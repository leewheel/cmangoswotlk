#ifndef _OBFUSCATOR_H
#define _OBFUSCATOR_H

#include <string>
#include <array>
#include <cstdint>

namespace Obfuscate
{
    constexpr char s_key = 'C';

    template<size_t N>
    struct ObfString
    {
        std::array<char, N> m_data;

        constexpr ObfString(const char(&str)[N]) noexcept : m_data{}
        {
            for (size_t i = 0; i < N; ++i)
                m_data[i] = str[i] ^ (s_key + static_cast<char>(i * 7 % 251));
        }

        std::string decode() const
        {
            std::string result;
            result.reserve(N - 1);
            for (size_t i = 0; i < N - 1; ++i)
                result.push_back(m_data[i] ^ (s_key + static_cast<char>(i * 7 % 251)));
            return result;
        }

        constexpr char operator[](size_t i) const { return m_data[i] ^ (s_key + static_cast<char>(i * 7 % 251)); }
    };

    template<size_t N>
    constexpr auto make_obf(const char(&str)[N]) noexcept
    {
        return ObfString<N>(str);
    }
}

#define OBFUSCATE(str) (Obfuscate::make_obf(str).decode())
#define OBFUSCATE_CSTR(str) []{ static const auto obf = Obfuscate::make_obf(str); thread_local static std::string cached; cached = obf.decode(); return cached.c_str(); }()

#endif
