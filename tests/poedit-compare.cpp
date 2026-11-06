// The same commands as tests/tmtool.c (insert, commit, rollback, search,
// sub), run on Poedit's own translation memory (gfamigapoedit's
// src/tm/transmem.cpp, Poedit's types stubbed), ids not shown: to check
// that GFOpenTransMem answers as it does. tests/compare-poedit.sh.
// MIT, Copyright (c) 2026 AmiCradle Team.
#include "transmem.h"
#include "str_helpers.h"
#include <cstdio>
#include <clocale>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
static std::vector<std::string> split(const std::string& l) {
  std::vector<std::string> v; size_t s = 0, t;
  while ((t = l.find('\t', s)) != std::string::npos) { v.push_back(l.substr(s, t - s)); s = t + 1; }
  v.push_back(l.substr(s)); return v; }
int main(int argc, char** argv) {
  setlocale(LC_ALL, "C.UTF-8");
  auto& tm = TranslationMemory::Get();
  auto w = tm.GetWriter();
  FILE* in = fopen(argv[1], "r"); char buf[4096];
  while (fgets(buf, sizeof buf, in)) {
    std::string l(buf); while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    auto f = split(l); while (f.size() < 6) f.push_back("");
    const std::string& c = f[0];
    if (c == "insert") w->Insert(Language(f[1]), Language(f[2]), str::to_wstring(f[3]), str::to_wstring(f[4]), (time_t)atol(f[5].c_str()) ? (time_t)atol(f[5].c_str()) : 1700000000);
    else if (c == "commit") w->Commit();
    else if (c == "rollback") w->Rollback();
    else if (c == "echo") printf("%s\n", f[1].c_str());
    else if (c == "search") {
      printf("Q: %s\n", f[3].c_str());
      for (auto& s : tm.Search(Language(f[1]), Language(f[2]), str::to_wstring(f[3])))
        printf("   %ld.%06ld  -> %s\n", (long)(s.score * 1e6 + 0.5) / 1000000, (long)(s.score * 1e6 + 0.5) % 1000000, str::to_utf8(s.text).c_str());
    } else if (c == "sub") {
      printf("Q: %s\n", f[3].c_str());
      struct P : TranslationMemory::IOInterface { void Insert(const Language&, const Language&, const std::wstring& s, const std::wstring& t, time_t) override {
        printf("   sub:  %s -> %s\n", str::to_utf8(s).c_str(), str::to_utf8(t).c_str()); } } p;
      tm.SearchSubstring(p, Language(f[1]), Language(f[2]), str::to_wstring(f[3]));
    }
  }
  TranslationMemory::CleanUp();
}
