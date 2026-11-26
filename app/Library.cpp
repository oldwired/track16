#include "app/Library.h"

#include "app/Song.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>

namespace app
{
namespace
{
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool skipFolder(const std::string& name)
{
    const auto n = lower(name);
    return name.empty() || name[0] == '.' || n == "system volume information" || n == "$recycle.bin"
        || (n.size() > 5 && n.compare(n.size() - 5, 5, "-data") == 0); // the boot menu's per-app data folders
}
} // namespace

void Library::scan(const std::vector<std::string>& roots, int depth, std::size_t limit)
{
    entries_.clear();
    root_.clear();
    for (const auto& r : roots)
    {
        struct stat st {};
        if (r.empty() || ::stat(r.c_str(), &st) != 0 || ! S_ISDIR(st.st_mode))
            continue;
        if (root_.empty())
            root_ = r;
        walk(r, "", depth, limit);
        break;
    }
    std::sort(entries_.begin(), entries_.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        const auto fa = lower(a.folder), fb = lower(b.folder);
        return fa != fb ? fa < fb : lower(a.name) < lower(b.name);
    });
}

void Library::walk(const std::string& dir, const std::string& rel, int depth, std::size_t limit)
{
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr)
        return;
    std::vector<std::string> folders;
    while (const dirent* e = ::readdir(d))
    {
        if (entries_.size() >= limit)
            break;
        const std::string name = e->d_name;
        if (name == "." || name == ".." || name[0] == '.')
            continue;
        const std::string full = dir + "/" + name;
        struct stat st {};
        if (::stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
        {
            if (depth > 0 && ! skipFolder(name))
                folders.push_back(name);
        }
        else if (S_ISREG(st.st_mode) && st.st_size > 0 && looksLikeModule(name))
            entries_.push_back({full, rel, name, static_cast<long>(st.st_size)});
    }
    ::closedir(d);
    for (const auto& f : folders)
        walk(dir + "/" + f, rel.empty() ? f : rel + "/" + f, depth - 1, limit);
}

int Library::indexOf(const std::string& path) const noexcept
{
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].path == path)
            return static_cast<int>(i);
    return -1;
}
} // namespace app
