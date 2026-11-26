// The module files on the stick: a recursive scan of the given folders (message thread, at start and on request).
// Hidden files and the app's own data folders are skipped. Entries are sorted by folder, then name (case-insensitive).
#pragma once

#include <string>
#include <vector>

namespace app
{
struct LibraryEntry
{
    std::string path;   // absolute
    std::string folder; // relative to the scan root ("" = the root)
    std::string name;   // file name
    long size = 0;
};

class Library
{
public:
    // Scans `roots` (the first existing one wins for relative folder names), at most `limit` files, `depth` levels.
    void scan(const std::vector<std::string>& roots, int depth = 4, std::size_t limit = 4000);
    const std::vector<LibraryEntry>& entries() const noexcept { return entries_; }
    const std::string& root() const noexcept { return root_; }
    int indexOf(const std::string& path) const noexcept;

private:
    void walk(const std::string& dir, const std::string& rel, int depth, std::size_t limit);
    std::vector<LibraryEntry> entries_;
    std::string root_;
};
} // namespace app
