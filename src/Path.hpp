// ===========================================================================
// The one thing about a file path that is not the same on three platforms.
//
// A file dialog on Windows hands back C:\Roms\Super Mario World.sfc, and one
// on Linux or macOS hands back /home/you/Roms/Super Mario World.sfc. Rack's
// own system::getEntries returns the forward-slash spelling on every
// platform, so a path that came out of a dialog and a path that came out of
// a directory listing do not compare equal on Windows -- which is what the
// shelf that < and > walk is doing -- and `find_last_of('/')` on a Windows
// path finds nothing at all, so the cartridge has no name and the folder it
// came from is empty.
//
// So: every path that enters this plugin from outside is put into the
// forward-slash spelling once, here, and nothing downstream has to think
// about it again. Windows has accepted a forward slash in a path since DOS
// 2.0, and so does nall's _wfopen, which is what actually opens the file.
//
// Header-only and free of both rack.hpp and nall, because it is used from
// the modules and from the seam that talks to bsnes, and those two are
// compiled by different makefiles with different include paths.
// ===========================================================================
#pragma once

#include <cstdio>
#include <string>

#if defined(_WIN32)
  #include <wchar.h>
#endif

namespace racksnes {

// Both spellings, because a path can arrive with either and Windows mixes
// them freely: "C:\Roms/Super Mario World.sfc" is a path Windows will open.
inline size_t lastSeparator(const std::string& path)
{
    return path.find_last_of("/\\");
}

// Backslashes to forward slashes. Called once, on the way in.
inline std::string normalizeSeparators(std::string path)
{
    for(char& c : path) if(c == '\\') c = '/';
    return path;
}

// "C:/Roms/Super Mario World.sfc" -> "Super Mario World.sfc"
inline std::string leafOf(const std::string& path)
{
    const size_t at = lastSeparator(path);
    return at == std::string::npos ? path : path.substr(at + 1);
}

// "C:/Roms/Super Mario World.sfc" -> "C:/Roms"
inline std::string folderOf(const std::string& path)
{
    const size_t at = lastSeparator(path);
    return at == std::string::npos ? std::string() : path.substr(0, at);
}

// A title on its way to becoming a filename. The set of characters a name
// may not contain is Windows's, which is the largest of the three, so using
// it everywhere costs a space in a filename that would have been legal on
// Linux and saves a save dialog that silently refuses.
inline std::string asFilename(std::string name)
{
    for(char& c : name)
        if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
        || c == '"' || c == '<' || c == '>' || c == '|')
            c = ' ';
    return name;
}

// ---------------------------------------------------------------------------
// Opening a file whose name is not in ASCII.
//
// Every path in this plugin is UTF-8: that is what Rack's file dialog hands
// back and what Rack's own filesystem calls take. On Linux and macOS that is
// also what fopen() takes, and there is nothing to do. On Windows fopen()
// takes the machine's ANSI code page, so a cartridge in a folder with an
// accent in it -- "Roms/Espa\xC3\xB1ol/..." -- is a file that does not exist,
// and the module reports that it could not read it. _wfopen takes UTF-16 and
// always works.
//
// The conversion is written out here rather than taken from MultiByteToWide-
// Char so that this header does not have to drag <windows.h> -- and its
// min/max macros -- into a Rack module.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
inline std::wstring toUtf16(const std::string& utf8)
{
    std::wstring out;
    out.reserve(utf8.size());

    for(size_t i = 0; i < utf8.size(); )
    {
        const unsigned char c = (unsigned char)utf8[i];
        unsigned long cp;
        int extra;

        if     (c < 0x80) { cp = c;        extra = 0; }
        else if(c < 0xE0) { cp = c & 0x1F; extra = 1; }
        else if(c < 0xF0) { cp = c & 0x0F; extra = 2; }
        else              { cp = c & 0x07; extra = 3; }

        // A truncated sequence at the end of the string is not something to
        // guess at: stop, and let the caller fail to open the file.
        if(i + (size_t)extra >= utf8.size()) break;
        ++i;

        for(int k = 0; k < extra; ++k, ++i)
            cp = (cp << 6) | ((unsigned char)utf8[i] & 0x3F);

        if(cp >= 0x10000)
        {
            cp -= 0x10000;
            out.push_back((wchar_t)(0xD800 + (cp >> 10)));
            out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
        }
        else out.push_back((wchar_t)cp);
    }

    return out;
}
#endif

// fopen, for a UTF-8 path, on all three platforms.
inline std::FILE* openBinary(const std::string& path, const char* mode)
{
#if defined(_WIN32)
    const std::wstring wide = toUtf16(path);
    const std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(wide.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

} // namespace racksnes
