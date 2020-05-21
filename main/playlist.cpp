#include "playlist.hpp"
#include <malloc.h>
#include <ctype.h>
#include <string.h>

void Playlist::clear()
{
    for (auto ptr: *this) {
        free(ptr);
    }
    Base::clear();
    mNextTrack = 0;
}
const char* Playlist::getNextTrack()
{
    if (empty()) {
        return nullptr;
    }
    if (mNextTrack >= size()) {
        mNextTrack = 0;
    }
    return at(mNextTrack++);
}

/** @returns pointer to first non-whitespace char, and pointer to the
 *  char before the first non-whitespace char on the next line.
 *  All chars after last char on line and before first char on next line
 *  are set to NULL
 */
char* Playlist::readLine(char*& start)
{
    for (;;start++) {
        char ch = *start;
        if (!ch) {
            return nullptr;
        } else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
           break;
        }
    }
    char* end = start + 1;
    for (;;end++) {
        char ch = *end;
        if (!ch) {
            while (isspace(*(--end))) *end = 0; // trim list right
            return nullptr;
        } else if (ch == '\r' || ch == '\n') { // trim line right
            *end = 0;
            while (isspace(*(--end))) *end = 0;
            return *end ? end+1 : end;
        }
    }
}

void Playlist::load(char* data)
{
    clear();
    if (!data) {
        return;
    }

    char* end;
    char* start = data;
    for(;;) {
        end = readLine(start);
        if (start[0] != '#') {
            push_back(strdup(start));
        }
        if (!end) {
            return;
        }
        start = end + 1;
    }
    handlePls();
}

bool Playlist::handlePls()
{
    if (empty() || strcasecmp(at(0), "[playlist]") != 0) {
        return false;
    }

    std::vector<char*> newList;
    for (int i = 1; i < size(); i++) {
        auto eq = strchr(at(i), '=');
        if (!eq) {
            continue;
        }
        while(isspace(*(++eq)));
        if (!eq) {
            continue;
        }
        if (strcasecmp(eq, "http") == 0) {
            newList.push_back(strdup(eq));
        }
    }
    clear();
    Base::operator=(newList);
    return true;
}
