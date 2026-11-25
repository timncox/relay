/*
 * picker_list — what the firmware picker lists, without FatFS.
 *
 * The card's /alchemy folder may hold .bin files and folders of .bin files
 * one level deep (say /alchemy/mutable/clouds_alchemy.bin). The picker reads
 * both into one flat, sorted list of entries:
 *
 *   file    a .bin at the top:          name "belt_alchemy.bin"
 *   folder  a folder holding .bin files: name "mutable"
 *   in      a .bin inside a folder:      name "mutable/clouds_alchemy.bin"
 *
 * Two views over it:
 *
 *   - the knob (FILE pot) sees one level at a time: at the top, the top files
 *     and one dot per folder; inside a folder, a "back" dot and that folder's
 *     files. So a ring never needs more dots than one level holds.
 *   - the app (HostLink FW_LIST) sees every file, both levels, by its path
 *     under /alchemy, because a list on a phone has room for all of them.
 *
 * Header-only and free of hardware so tests/picker_list_test.cpp can check
 * it on a computer.
 */
#pragma once

#include <stdint.h>
#include <string.h>
#include <strings.h>

namespace picker_list {

constexpr int kMaxEntries = 40;   /* files + folders, both levels */
constexpr int kMaxFolders = 6;
constexpr int kNameLen    = 48;   /* "folder/name.bin" + NUL */
constexpr int kMaxView    = 20;   /* dots on the knob at one level, back dot included */

enum Kind : uint8_t { kFile = 0, kFolder = 1, kIn = 2 };

struct List
{
    int      count = 0;
    char     name[kMaxEntries][kNameLen] = {};
    uint32_t size[kMaxEntries] = {};
    uint8_t  kind[kMaxEntries] = {};

    /* App view: entry indexes of every file, both levels, in sorted order. */
    int files[kMaxEntries] = {};
    int nfiles = 0;

    /* Knob view: entry indexes at the current level; -1 is the back dot. */
    char cur[kNameLen] = {};   /* "" = top; else a folder name */
    int  view[kMaxView] = {};
    int  nview = 0;

    void Clear() { count = 0; nfiles = 0; nview = 0; }

    bool IsBin(const char* fn) const
    {
        const size_t L = strlen(fn);
        return L >= 5 && strcasecmp(fn + L - 4, ".bin") == 0;
    }

    /* Hidden and AppleDouble ("._x.bin") names are never firmware. */
    static bool Skippable(const char* fn) { return fn[0] == '\0' || fn[0] == '.'; }

    /* A top-level .bin. Returns false when it was not taken. */
    bool AddFile(const char* fn, uint32_t sz)
    {
        if (Skippable(fn) || !IsBin(fn)) return false;
        return put(fn, nullptr, sz, kFile);
    }

    /* A .bin inside folder `dir`. The folder's own entry is added with its
     * first file, so an empty folder never shows a dot. */
    bool AddIn(const char* dir, const char* fn, uint32_t sz)
    {
        if (Skippable(fn) || !IsBin(fn) || Skippable(dir)) return false;
        if (strlen(dir) + 1 + strlen(fn) > (size_t)kNameLen - 1) return false;
        if (find(dir, kFolder) < 0)
        {
            if (folders() >= kMaxFolders) return false;
            if (count + 2 > kMaxEntries) return false;   /* room for folder + file */
            put(dir, nullptr, 0, kFolder);
        }
        return put(dir, fn, sz, kIn);
    }

    /* Alphabetical (case-insensitive) by full name, then rebuild both views.
     * A folder "mutable" sorts before its "mutable/..." files. */
    void Finish()
    {
        for (int i = 1; i < count; i++)
            for (int j = i; j > 0 && strcasecmp(name[j - 1], name[j]) > 0; j--)
                swap(j, j - 1);
        nfiles = 0;
        for (int i = 0; i < count; i++)
            if (kind[i] != kFolder) files[nfiles++] = i;
        if (cur[0] && find(cur, kFolder) < 0) cur[0] = '\0';   /* folder gone */
        BuildView();
    }

    void BuildView()
    {
        nview = 0;
        if (cur[0])
        {
            view[nview++] = -1;   /* back */
            for (int i = 0; i < count && nview < kMaxView; i++)
                if (kind[i] == kIn && in_folder(i, cur)) view[nview++] = i;
        }
        else
        {
            for (int i = 0; i < count && nview < kMaxView; i++)
                if (kind[i] != kIn) view[nview++] = i;
        }
    }

    void Enter(int entry)
    {
        if (entry < 0 || entry >= count || kind[entry] != kFolder) return;
        strncpy(cur, name[entry], kNameLen - 1);
        cur[kNameLen - 1] = '\0';
        BuildView();
    }

    void Up()
    {
        cur[0] = '\0';
        BuildView();
    }

    /* App lookup by path under /alchemy. */
    int FindFile(const char* path) const
    {
        for (int k = 0; k < nfiles; k++)
            if (strcmp(name[files[k]], path) == 0) return files[k];
        return -1;
    }

    /* What the knob shows for view slot v: the name inside the current
     * folder (no folder prefix), or "" for the back dot. */
    const char* ViewLabel(int v) const
    {
        if (v < 0 || v >= nview || view[v] < 0) return "";
        const char* n = name[view[v]];
        const char* s = strrchr(n, '/');
        return s ? s + 1 : n;
    }

  private:
    int folders() const
    {
        int n = 0;
        for (int i = 0; i < count; i++) n += kind[i] == kFolder;
        return n;
    }

    int find(const char* n, uint8_t k) const
    {
        for (int i = 0; i < count; i++)
            if (kind[i] == k && strcasecmp(name[i], n) == 0) return i;
        return -1;
    }

    bool in_folder(int i, const char* dir) const
    {
        const size_t L = strlen(dir);
        return strncasecmp(name[i], dir, L) == 0 && name[i][L] == '/';
    }

    bool put(const char* a, const char* b, uint32_t sz, uint8_t k)
    {
        if (count >= kMaxEntries) return false;
        const size_t la = strlen(a), lb = b ? strlen(b) : 0;
        const size_t L  = la + (b ? 1 + lb : 0);
        if (L > (size_t)kNameLen - 1) return false;   /* too long to reopen */
        memcpy(name[count], a, la);
        if (b)
        {
            name[count][la] = '/';
            memcpy(name[count] + la + 1, b, lb);
        }
        name[count][L] = '\0';
        size[count]    = sz;
        kind[count]    = k;
        count++;
        return true;
    }

    void swap(int a, int b)
    {
        char t[kNameLen];
        memcpy(t, name[a], kNameLen);
        memcpy(name[a], name[b], kNameLen);
        memcpy(name[b], t, kNameLen);
        const uint32_t ts = size[a]; size[a] = size[b]; size[b] = ts;
        const uint8_t  tk = kind[a]; kind[a] = kind[b]; kind[b] = tk;
    }
};

} // namespace picker_list
