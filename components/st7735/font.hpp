#ifndef FONT_HPP
#define FONT_HPP
#include <stdint.h>

struct Font
{
    const uint8_t width;
    const uint8_t height;
    const uint8_t charCount;
    const uint8_t charSpacing: 4;
    const uint8_t lineSpacing: 4;
    const uint8_t* offsets;
    const uint8_t* data;
    const bool isVertScan;
    const uint8_t byteHeightOrWidth;
    Font(bool aVert, uint8_t aWidth, uint8_t aHeight, uint8_t aCount, uint8_t charSp,
         uint8_t lineSp, const void* aData, const uint8_t* aOffsets=nullptr)
    :width(aWidth), height(aHeight), charCount(aCount), charSpacing(charSp),
     lineSpacing(lineSp), offsets(aOffsets), data((uint8_t*)aData), isVertScan(aVert),
     byteHeightOrWidth(isVertScan ? ((aHeight + 7) / 8) : (aWidth + 7) / 8)
    {}
    bool isMono() const { return offsets == nullptr; }
    int codeToIdx(uint8_t code) const {
        if (code < 32) {
            return -1;
        }
        code -= 32;
        return (code >= charCount) ? -1 : code;
    }
    const uint8_t* getCharData(uint8_t& code) const
    {
        int idx = codeToIdx(code);
        if (idx < 0) {
            return nullptr;
        }
        if (isVertScan) {
            if (!offsets) {
                code = width;
                return data + (byteHeightOrWidth * width) * idx;
            }
            else {
                auto ofs = (idx == 0) ? 0 : offsets[idx-1];
                code = (offsets[idx] - ofs) / byteHeightOrWidth;
                return data + ofs;
            }
        } else {
            // only monospace fonts supported
            code = width;
            return data + (byteHeightOrWidth * height) * idx;
        }
    }
    int charWidth(char ch=0) const {
        if (!offsets) {
            return width;
        }
        auto idx = codeToIdx(ch);
        if (idx < 0) {
            return 0;
        }
        auto ofs = (idx == 0) ? 0 : offsets[idx - 1];
        return (offsets[idx] - ofs) / byteHeightOrWidth;
    }
};

#endif // FONT_HPP
