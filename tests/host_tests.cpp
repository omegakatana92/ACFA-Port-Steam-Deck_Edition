// Host-side tests of portable code. No game data and no graphics: these run on the build host
// (Windows or Linux) and check the primitives the preparation tools and runtime depend on.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/crypto.h"
#include "common/file.h"
#include "common/ini.h"

namespace fs = std::filesystem;
namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
  }
}

std::string hex_of(const void* p, size_t n) { return acfa::hex(p, n); }

void test_hashes() {
  auto sha1_abc = acfa::crypto::sha1("abc", 3);
  check(hex_of(sha1_abc.data(), sha1_abc.size()) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1('abc')");
  auto sha1_empty = acfa::crypto::sha1("", 0);
  check(hex_of(sha1_empty.data(), sha1_empty.size()) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1('')");

  auto sha256_abc = acfa::crypto::sha256("abc", 3);
  check(hex_of(sha256_abc.data(), sha256_abc.size()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256('abc')");
  auto sha256_empty = acfa::crypto::sha256("", 0);
  check(hex_of(sha256_empty.data(), sha256_empty.size()) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256('')");

  // Multi-block inputs exercise the incremental padding path.
  std::vector<char> million(1000000, 'a');
  auto sha1_million = acfa::crypto::sha1(million.data(), million.size());
  check(hex_of(sha1_million.data(), sha1_million.size()) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
        "SHA-1(1e6 'a')");
  auto sha256_million = acfa::crypto::sha256(million.data(), million.size());
  check(hex_of(sha256_million.data(), sha256_million.size()) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "SHA-256(1e6 'a')");
}

void test_file_hash(const fs::path& dir) {
  fs::path p = dir / "hash-input.bin";
  {
    std::ofstream f(p, std::ios::binary);
    f << "abc";
  }
  check(acfa::crypto::sha256_file_hex(p.string()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256_file_hex");
  std::error_code ec;
  fs::remove(p, ec);
}

void test_guard(const fs::path& dir) {
  fs::path root = dir / "read-only-root";
  fs::create_directories(root / "PS3_GAME");
  acfa::add_protected_root(root);
  bool threw = false;
  try {
    acfa::write_text(root / "PS3_GAME" / "x.txt", "nope");
  } catch (const std::exception&) {
    threw = true;
  }
  check(threw, "guard_write refuses a protected root");
  acfa::write_text(dir / "allowed.txt", "ok");
  check(fs::exists(dir / "allowed.txt"), "guard_write allows paths outside protected roots");

  // The same prefix without a separator must not be considered inside the root.
  fs::path sibling = dir / "read-only-root-sibling";
  acfa::write_text(sibling / "allowed.txt", "ok");
  check(fs::exists(sibling / "allowed.txt"), "guard_write does not match a mere path prefix");
}

void test_ini(const fs::path& dir) {
  acfa::Ini ini = acfa::Ini::parse("[display]\r\nmode = borderless\r\nfps = 60\r\n");
  check(ini.get_or("display", "mode", "") == "borderless", "INI parse");
  ini.set("display", "fps", "120");
  ini.set("input", "forward", "W");
  fs::path p = dir / "settings.ini";
  ini.save(p);
  acfa::Ini back = acfa::Ini::load(p);
  check(back.get_or("display", "fps", "") == "120", "INI save/load changes a value");
  check(back.get_or("input", "forward", "") == "W", "INI save/load adds a section");
  check(!fs::exists(p.string() + ".tmp"), "INI save removes its temporary file");
}

}  // namespace

int main() {
  fs::path dir = fs::temp_directory_path() / "acfa-host-tests";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir);

  test_hashes();
  test_file_hash(dir);
  test_guard(dir);
  test_ini(dir);

  fs::remove_all(dir, ec);
  if (g_failures) {
    std::fprintf(stderr, "%d test(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all host tests passed\n");
  return 0;
}
