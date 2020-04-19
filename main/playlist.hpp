#ifndef PLAYLIST_HPP
#define PLAYLIST_HPP
#include <vector>
class Playlist: public std::vector<char*>
{
    typedef std::vector<char*> Base;
    uint16_t mNextTrack = 0;
    char* readLine(char*& start);
    bool handlePls();
public:
    void clear();
    void load(char* data);
    const char* getNextTrack();
};

#endif // PLAYLIST_HPP
