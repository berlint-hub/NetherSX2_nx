#include "fps_mods.h"

#include <curl/curl.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kMaxDownloadedBytes = 1024 * 1024;
constexpr size_t kMaxPnachBytes = 4 * 1024 * 1024;
constexpr const char *kInstalledMarker = "[FPS Mod: 30 FPS]";
constexpr const char *kDisabledMarker = "// [NetherSX2-nx disabled] ";

struct CatalogEntry {
  uint32_t crc;
  const char *label;
  const char *url;
};

// Only entries whose patch section and game CRC have been checked are exposed.
// This first entry is the official PCSX2 patch database's MGS3 Subsistence
// (NTSC-U, disc 1) 30 FPS section. GT3 is intentionally not listed: the
// inspected GT3 patch file contains no frame-rate patch.
constexpr CatalogEntry kCatalog[] = {
    {0x053D2239u, "Metal Gear Solid 3: Subsistence (NTSC-U) — 30 FPS",
     "https://raw.githubusercontent.com/PCSX2/pcsx2_patches/ba8ef4f42c353d8ee44d572f8111b10db54a3f0b/patches/SLUS-21359_053D2239.pnach"},
};

const CatalogEntry *find_entry(uint32_t crc) {
  for (const CatalogEntry &entry : kCatalog) {
    if (entry.crc == crc) return &entry;
  }
  return nullptr;
}

size_t receive_bytes(void *data, size_t size, size_t count, void *user) {
  if (size != 0 && count > static_cast<size_t>(-1) / size) return 0;
  const size_t bytes = size * count;
  std::string *body = static_cast<std::string *>(user);
  if (bytes > kMaxDownloadedBytes - body->size()) return 0;
  body->append(static_cast<const char *>(data), bytes);
  return bytes;
}

bool download(const char *url, std::string *body) {
  CURL *curl = curl_easy_init();
  if (!curl) return false;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 4L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_bytes);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "NetherSX2-NX-FPS-Mods/1.0");
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  return result == CURLE_OK && status >= 200 && status < 300 && !body->empty();
}

std::string trim(const std::string &line) {
  size_t first = 0;
  while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first])))
    first++;
  size_t last = line.size();
  while (last > first && std::isspace(static_cast<unsigned char>(line[last - 1])))
    last--;
  return line.substr(first, last - first);
}

bool is_30_fps_section(const std::string &line) {
  const std::string value = trim(line);
  if (value.empty() || value.front() != '[' || value.back() != ']') return false;
  const char expected[] = "[30 FPS]";
  if (value.size() != sizeof(expected) - 1) return false;
  for (size_t i = 0; i < value.size(); i++) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(expected[i])))
      return false;
  }
  return true;
}

bool is_section_header(const std::string &line) {
  const std::string value = trim(line);
  return value.size() >= 3 && value.front() == '[' && value.back() == ']';
}

bool valid_patch_line(const std::string &line) {
  const std::string value = trim(line);
  if (value.rfind("patch=1,EE,", 0) != 0) return false;
  // Refuse unexpected directives or multi-line payloads from a changed source.
  if (value.find_first_of("\r\n") != std::string::npos) return false;
  const size_t first = value.find(',', 11);
  const size_t second = first == std::string::npos ? first : value.find(',', first + 1);
  return first != std::string::npos && second != std::string::npos;
}

bool extract_30_fps_patches(const std::string &source, std::vector<std::string> *patches) {
  patches->clear();
  bool in_target = false;
  size_t position = 0;
  while (position <= source.size()) {
    const size_t end = source.find('\n', position);
    const size_t length = (end == std::string::npos ? source.size() : end) - position;
    std::string line = source.substr(position, length);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (is_section_header(line)) {
      if (in_target) break;
      in_target = is_30_fps_section(line);
    } else if (in_target) {
      const std::string value = trim(line);
      if (value.rfind("patch=", 0) == 0) {
        if (!valid_patch_line(value) || patches->size() >= 32) {
          patches->clear();
          return false;
        }
        patches->push_back(value);
      }
    }

    if (end == std::string::npos) break;
    position = end + 1;
  }
  return in_target && !patches->empty();
}

bool has_existing_30_fps_section(const std::string &contents) {
  size_t position=0;
  while(position<=contents.size()){
    const size_t end=contents.find('\n',position);
    const size_t length=(end==std::string::npos?contents.size():end)-position;
    if(is_30_fps_section(contents.substr(position,length))) return true;
    if(end==std::string::npos) break;
    position=end+1;
  }
  return false;
}

bool recover_install(const char *path) {
  const std::string temporary=std::string(path)+".fps.tmp";
  const std::string backup=std::string(path)+".fps.old";
  struct stat current_info{},backup_info{};
  const bool current=stat(path,&current_info)==0;
  const int current_error=current?0:errno;
  const bool old=stat(backup.c_str(),&backup_info)==0;
  const int old_error=old?0:errno;
  if((!current&&current_error!=ENOENT)||(!old&&old_error!=ENOENT)) return false;
  bool changed=false;
  if(!current&&old){
    if(std::rename(backup.c_str(),path)!=0) return false;
    changed=true;
  } else if(current&&old){
    if(std::remove(backup.c_str())!=0) return false;
    changed=true;
  }
  if(std::remove(temporary.c_str())==0) changed=true;
  else if(errno!=ENOENT) return false;
#ifdef __SWITCH__
  if(changed) fsdevCommitDevice("sdmc");
#else
  (void)changed;
#endif
  return true;
}

bool read_existing(const char *path, std::string *contents) {
  contents->clear();
  FILE *file = std::fopen(path, "rb");
  if (!file) return errno == ENOENT;
  struct stat info {};
  if (fstat(fileno(file), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      static_cast<size_t>(info.st_size) > kMaxPnachBytes) {
    std::fclose(file);
    return false;
  }
  contents->resize(static_cast<size_t>(info.st_size));
  const bool ok = contents->empty() ||
                  std::fread(&(*contents)[0], 1, contents->size(), file) == contents->size();
  std::fclose(file);
  return ok;
}

bool write_atomically(const char *path, const std::string &contents) {
  const std::string temporary = std::string(path) + ".fps.tmp";
  const std::string backup = std::string(path) + ".fps.old";
  std::remove(temporary.c_str());
  FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) return false;
  const bool wrote = contents.empty() ||
                     std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
  const bool flushed = wrote && std::fflush(file) == 0 && fsync(fileno(file)) == 0;
  const bool closed = std::fclose(file) == 0;
  if (!flushed || !closed) {
    std::remove(temporary.c_str());
    return false;
  }

  struct stat info {};
  const bool existed = stat(path, &info) == 0;
  if (!existed && errno != ENOENT) {
    std::remove(temporary.c_str());
    return false;
  }
  std::remove(backup.c_str());
  if (existed && std::rename(path, backup.c_str()) != 0) {
    std::remove(temporary.c_str());
    return false;
  }
  if (std::rename(temporary.c_str(), path) != 0) {
    if (existed) std::rename(backup.c_str(), path);
    std::remove(temporary.c_str());
    return false;
  }
  if (existed) std::remove(backup.c_str());
#ifdef __SWITCH__
  fsdevCommitDevice("sdmc");
#endif
  return true;
}

} // namespace

extern "C" int nx_fps_mod_available(uint32_t crc) {
  return find_entry(crc) != nullptr;
}

extern "C" int nx_fps_mod_installed(uint32_t crc, const char *pnach_path) {
  if (!find_entry(crc) || !pnach_path || !recover_install(pnach_path)) return 0;
  std::string contents;
  if (!read_existing(pnach_path, &contents)) return 0;
  return contents.find(kInstalledMarker) != std::string::npos ||
         has_existing_30_fps_section(contents);
}

extern "C" const char *nx_fps_mod_label(uint32_t crc) {
  const CatalogEntry *entry = find_entry(crc);
  return entry ? entry->label : nullptr;
}

extern "C" NxFpsModInstallResult nx_fps_mod_install(uint32_t crc, const char *pnach_path) {
  const CatalogEntry *entry = find_entry(crc);
  if (!entry || !pnach_path) return NX_FPS_MOD_UNSUPPORTED_GAME;
  if (!recover_install(pnach_path)) return NX_FPS_MOD_FILE_ERROR;

  std::string existing;
  if (!read_existing(pnach_path, &existing)) return NX_FPS_MOD_FILE_ERROR;
  if (existing.find(kInstalledMarker) != std::string::npos ||
      has_existing_30_fps_section(existing))
    return NX_FPS_MOD_ALREADY_INSTALLED;

  std::string source;
  if (!download(entry->url, &source)) return NX_FPS_MOD_NETWORK_ERROR;
  std::vector<std::string> patches;
  if (!extract_30_fps_patches(source, &patches)) return NX_FPS_MOD_INVALID_SOURCE;

  std::string updated = existing;
  if (!updated.empty() && updated.back() != '\n') updated += '\n';
  if (!updated.empty()) updated += '\n';
  updated += std::string(kInstalledMarker) + "\n";
  updated += "// Source: PCSX2/pcsx2_patches; imported disabled; enable in Cheat codes.\n";
  for (const std::string &patch : patches)
    updated += kDisabledMarker + patch + "\n";
  if (updated.size() > kMaxPnachBytes) return NX_FPS_MOD_FILE_ERROR;
  if (!write_atomically(pnach_path, updated)) return NX_FPS_MOD_FILE_ERROR;
  return NX_FPS_MOD_INSTALLED;
}
