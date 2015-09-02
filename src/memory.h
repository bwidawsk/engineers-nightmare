#pragma once

static inline
size_t align_size(size_t s, size_t align)
{
    s += align-1;
    s &= ~(align-1);
    return s;
}

static inline
void * align_ptr(void *p, size_t align)
{
    return (void *)align_size((size_t)p, align);
}

template<typename T>
size_t align_size(size_t s)
{
    return align_size(s, alignof(T));
}

template<typename T>
T* align_ptr(T* p) {
    return (T*)align_size<T>((size_t)p);
}