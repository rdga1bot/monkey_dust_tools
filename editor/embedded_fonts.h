#pragma once
// embedded_fonts.h — extern declarations of embedded TTF byte arrays.
// Actual data is in embedded_fonts.cpp (compiled once into engine lib).
#include <cstdint>

namespace md_font {
extern const uint32_t Arimo_Regular_size;
extern const uint8_t  Arimo_Regular[];
extern const uint32_t Arimo_Bold_size;
extern const uint8_t  Arimo_Bold[];
extern const uint32_t Arimo_Italic_size;
extern const uint8_t  Arimo_Italic[];
extern const uint32_t Arimo_BoldItalic_size;
extern const uint8_t  Arimo_BoldItalic[];
extern const uint32_t UbuntuMono_Regular_size;
extern const uint8_t  UbuntuMono_Regular[];
extern const uint32_t UbuntuMono_Bold_size;
extern const uint8_t  UbuntuMono_Bold[];
extern const uint32_t UbuntuMono_Italic_size;
extern const uint8_t  UbuntuMono_Italic[];
extern const uint32_t UbuntuMono_BoldItalic_size;
extern const uint8_t  UbuntuMono_BoldItalic[];

} // namespace md_font