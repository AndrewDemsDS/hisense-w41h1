// Replay driver: calls a harness's LLVMFuzzerTestOneInput on every file named on the command
// line. A directory means every regular file in it (not recursive). A file whose name ends in
// ".hex" is a packed corpus: one input per line, as hex digits (corpus/<harness>.hex, written by
// run.sh --merge; text, so the corpus can be read and diffed). It stands in for libFuzzer in
// the smoke run and the coverage run, so those need nothing but a C++17 compiler with the
// sanitizers, g++ included. A broken property abort()s, a sanitizer report exits non-zero, and
// either way the name of the input being replayed is the last "replay" line printed.
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void run_bytes(const std::string &name, const std::vector<uint8_t> &bytes) {
  // Say which input is running before running it: if it crashes, this is the line that names it.
  fprintf(stderr, "replay %s (%zu bytes)\n", name.c_str(), bytes.size());
  // An exact heap copy, so a harness that reads past its input is caught too.
  uint8_t *copy = static_cast<uint8_t *>(malloc(bytes.empty() ? 1 : bytes.size()));
  if (copy == nullptr)
    abort();
  for (size_t i = 0; i < bytes.size(); i++)
    copy[i] = bytes[i];
  LLVMFuzzerTestOneInput(copy, bytes.size());
  free(copy);
}

static int hex_digit(int c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Returns the number of inputs replayed from `path`.
static size_t run_file(const std::string &path) {
  FILE *fp = fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    fprintf(stderr, "cannot open %s\n", path.c_str());
    return 0;
  }
  std::vector<uint8_t> bytes;
  uint8_t chunk[4096];
  size_t got;
  while ((got = fread(chunk, 1, sizeof(chunk), fp)) > 0)
    bytes.insert(bytes.end(), chunk, chunk + got);
  fclose(fp);

  const bool packed = path.size() > 4 && path.compare(path.size() - 4, 4, ".hex") == 0;
  if (!packed) {
    run_bytes(path, bytes);
    return 1;
  }
  size_t count = 0, line = 1;
  std::vector<uint8_t> input;
  int high = -1;
  bool comment = false, any = false;
  bytes.push_back('\n');
  for (uint8_t c : bytes) {
    if (c == '\n') {
      // An empty line is the empty input only when written as "-"; blank lines are skipped.
      if (any && !comment) {
        run_bytes(path + ":" + std::to_string(line), input);
        count++;
      }
      input.clear();
      high = -1;
      comment = any = false;
      line++;
      continue;
    }
    if (comment)
      continue;
    if (c == '#') {
      comment = true;
      continue;
    }
    if (c == '-') {
      any = true;
      continue;
    }
    int d = hex_digit(c);
    if (d < 0)
      continue;
    any = true;
    if (high < 0) {
      high = d;
    } else {
      input.push_back(static_cast<uint8_t>((high << 4) | d));
      high = -1;
    }
  }
  return count;
}

int main(int argc, char **argv) {
  size_t count = 0;
  for (int i = 1; i < argc; i++) {
    struct stat st;
    if (stat(argv[i], &st) != 0)
      continue;  // a corpus directory that does not exist yet is not an error
    if (S_ISDIR(st.st_mode)) {
      DIR *d = opendir(argv[i]);
      if (d == nullptr)
        continue;
      std::vector<std::string> names;
      while (struct dirent *e = readdir(d)) {
        std::string path = std::string(argv[i]) + "/" + e->d_name;
        struct stat fs;
        if (stat(path.c_str(), &fs) == 0 && S_ISREG(fs.st_mode))
          names.push_back(path);
      }
      closedir(d);
      std::sort(names.begin(), names.end());  // readdir order is not stable
      for (const std::string &p : names)
        count += run_file(p);
    } else {
      count += run_file(argv[i]);
    }
  }
  printf("%zu inputs replayed\n", count);
  return 0;
}
