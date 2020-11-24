#ifndef FONT_HPP
#define FONT_HPP
#include <stdint.h>

struct Font
{
    const uint8_t width;
    const uint8_t height;
    const uint8_t charCount;
    uint8_t charSpacing: 4;
    uint8_t lineSpacing: 4;
    const uint8_t* widths;
    const uint8_t* data;
    bool isVertScan;
    Font(bool aVert, uint8_t aWidth, uint8_t aHeight, uint8_t aCount, uint8_t charSp,
         uint8_t lineSp, const void* aData, const uint8_t* aWidths=nullptr)
    :width(aWidth), height(aHeight), charCount(aCount), charSpacing(charSp),
     lineSpacing(lineSp), widths(aWidths), data((uint8_t*)aData), isVertScan(aVert)
    {}
    bool isMono() const { return widths == nullptr; }
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
            if (!widths) {
                uint8_t byteHeight = (height + 7) / 8;
                code = width;
                return data + (byteHeight * width) * idx;
            }
            else {
                uint32_t ofs = 0;
                for (int ch = 0; ch < idx; ch++) {
                    ofs += widths[ch];
                }
                code = widths[idx];
                return data + ofs;
            }
        } else {
            uint8_t byteWidth = (width + 7) / 8;
            code = width;
            return data + (byteWidth * height) * idx;
        }
    }
    int charWidth(char ch=0) const {
        if (!widths) {
            return width;
        }
        auto idx = codeToIdx(ch);
        return (idx < 0) ? 0 : widths[idx];
    }
};

#endif // FONT_HPP
