#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdio>

// snprintf returns the length it WANTED to write. Accumulating that value after
// truncation moves the next destination past its buffer and underflows capacity.
// Return only bytes actually appended, excluding the terminator, so a caller's
// offset remains at most capacity - 1 even after repeated truncated appends.
#if defined(__clang__) || defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
inline int BoundedSnprintf(char* destination, std::size_t capacity,
                           const char* format, ...)
{
    if (!capacity) return 0;
    std::va_list arguments;
    va_start(arguments, format);
    const int required = std::vsnprintf(destination, capacity, format, arguments);
    va_end(arguments);
    if (required < 0)
    {
        destination[0] = '\0';
        return 0;
    }
    return static_cast<std::size_t>(required) >= capacity
        ? static_cast<int>(capacity - 1) : required;
}
