#ifndef MY_ALLOCATOR_H
#define MY_ALLOCATOR_H
#include <cstddef>
#include <mutex>
#include <iostream>
#include <cstring>






//封装了 malloc 和 free 操作， 可以设置 OOM 释放内存的回调函数
// 分配失败了， 会尝试释放内存， 然后再次分配
template <int __inst>
class malloc_alloc_template {

private:
  static void* oom_malloc(size_t);
  static void* oom_realloc(void*, size_t);
  static void (* malloc_alloc_oom_handler)();

public:

  static void* allocate(size_t n)
  {
    void* result = malloc(n);
    if (0 == result) result = oom_malloc(n);
    return result;
  }

  static void deallocate(void* p, size_t /* __n */)
  {
    free(p);
  }

  static void* reallocate(void* p, size_t /* old_sz */, size_t new_sz)
  {
    void* result = realloc(p, new_sz);
    if (0 == result) result = oom_realloc(p, new_sz);
    return result;
  }

  static void (* set_malloc_handler(void (*f)()))()
  {
    void (* old)() = malloc_alloc_oom_handler;
      malloc_alloc_oom_handler = f;
    return(old);
  }

};


template <int __inst>
void*
malloc_alloc_template<__inst>::oom_malloc(size_t n)
{
    void (* my_malloc_handler)();
    void* result;

    for (;;) {
        my_malloc_handler = malloc_alloc_oom_handler;
        if (0 == my_malloc_handler) { throw std::bad_alloc(); }
        (*my_malloc_handler)();
        result = malloc(n);
        if (result) return(result);
    }
}

template <int inst>
void* malloc_alloc_template<inst>::oom_realloc(void* p, size_t n)
{
    void (* my_malloc_handler)();
    void* result;

    for (;;) {
        my_malloc_handler = malloc_alloc_oom_handler;
        if (0 == my_malloc_handler) { throw std::bad_alloc(); }
        (*my_malloc_handler)();
        result = realloc(p, n);
        if (result) return(result);
    }
}


typedef malloc_alloc_template<0> malloc_alloc;
template <int __inst>
void (* malloc_alloc_template<__inst>::malloc_alloc_oom_handler)() = nullptr;

template <typename T>
class MyAllocator
{
public:
    //容器要求的类型
    using value_type = T;

    constexpr MyAllocator() noexcept
    {
        //do nothing
    }
    constexpr MyAllocator (const MyAllocator&) noexcept = default;
    template<class _Other>
    constexpr MyAllocator(const MyAllocator<_Other>&) noexcept
    {
        //do nothing
    }

   static T* allocate(size_t n)
    {
        n = n * sizeof(T);
        void* ret = 0;


        if (n > (size_t) MAX_BYTES) {
            ret = malloc_alloc::allocate(n);
        }
        else {
        Obj* volatile* my_free_list
            = free_list + freelist_index(n);

        std::lock_guard<std::mutex> guard(mtx) ;
        Obj*  result = *my_free_list;
            if (result == 0)
                ret = refill(round_up(n));
            else {
                *my_free_list = result -> free_list_next;
                ret = result;
            }
        }
        return (T*)ret;
    }

    static void deallocate(void* p, size_t n)
    {
    /* p may not be 0 */
        if (n > (size_t) MAX_BYTES)
        {
            malloc_alloc::deallocate(p, n);
        }
        else
        {
        Obj* volatile*  my_free_list
            = free_list + freelist_index(n);
        Obj* q = (Obj*)p;

        std::lock_guard<std::mutex> guard(mtx) ;

            q -> free_list_next = *my_free_list;
        *my_free_list = q;
        // lock is released here
        }
        }

    static void* reallocate(void* p, size_t old_sz, size_t new_sz)
    {

        void* result;
        size_t copy_sz;

        if (old_sz > (size_t) MAX_BYTES && new_sz > (size_t) MAX_BYTES) {
            return(realloc(p, new_sz));
        }
        if (round_up(old_sz) == round_up(new_sz)) return(p);
        result = allocate(new_sz);
        copy_sz = new_sz > old_sz ? old_sz : new_sz;
        std::memcpy(result, p, copy_sz);
        deallocate(p, old_sz);
        return(result);

    }

    //object constructor
    void construct(T* p, const T& val)
    {
        new (p) T(val);
    }
    //object destructor
    void destroy(T* p)
    {
        p->~T();
    }
private:
    enum {ALIGN = 8};  // 自由链表从8字节开始, 以8字节为单位增长
    enum {MAX_BYTES = 128}; // 内存池最大128字节
    enum {NFREELISTS = 16}; //自由链表的个数



    //每一个chunk 块的头信息, _M_free_list_link指向下一个chunk块, 相当于链表的 next指针
    union Obj {
        union Obj* free_list_next;
        char client_data[1];    /* The client sees this.        */
    };

    // 已分配的内存 chunk 块的使用情况
    static char* start_free;
    static char* end_free;
    static size_t heap_size;

    //静态数组: 16个自由链表, 每个链表的头指针
    //volatile 防止编译器优化, 保证每次读取都是从内存中读取, 而不是从寄存器中读取
    static Obj* volatile  free_list[NFREELISTS];

    //内存池基于freelist的实现, 需要考虑线程安全
    static std::mutex mtx;


    //将__bytes上调至最临近的8的倍数
    static size_t round_up(size_t bytes)
    {
        return (((bytes) + (size_t) ALIGN - 1) & ~((size_t) ALIGN - 1));
    }

    //返回__bytes在自由链表中的索引
    static size_t freelist_index(size_t bytes)
    {
        return (((bytes) + (size_t)ALIGN - 1) / (size_t)ALIGN - 1);
    }
    //主要是把分配好的 chunk 块进行连接的
    static void* refill(size_t n)
    {
        int nobjs = 20;
        char* chunk = chunk_alloc(n, nobjs);
        Obj* volatile* my_free_list;
        Obj* result;
        Obj* current_obj;
        Obj* next_obj;
        int i;

        if (1 == nobjs) return(chunk);
        my_free_list = free_list + freelist_index(n);

        /* Build free list in chunk */
        result = (Obj*)chunk;
        *my_free_list = next_obj = (Obj*)(chunk + n);
        for (i = 1; ; i++) {
            current_obj = next_obj;
            next_obj = (Obj*)((char*)next_obj + n);
            if (nobjs - 1 == i) {
                current_obj -> free_list_next = 0;
                break;
            } else {
                current_obj -> free_list_next = next_obj;
            }
        }
        return(result);
    }
    //主要负责分配自由链表, chunk 块
    static char* chunk_alloc(size_t size, int& nobjs)
    {
        char* result;
        size_t total_bytes = size * nobjs;
        size_t bytes_left = end_free - start_free;

        if (bytes_left >= total_bytes) {
            result = start_free;
            start_free += total_bytes;
            return(result);
        } else if (bytes_left >= size) {
            nobjs = (int)(bytes_left / size);
            total_bytes = size * nobjs;
            result = start_free;
            start_free += total_bytes;
            return(result);
        } else {
            size_t __bytes_to_get =
                    2 * total_bytes + round_up(heap_size >> 4);
            // Try to make use of the left-over piece.
            if (bytes_left > 0) {
                Obj* volatile* __my_free_list =
                        free_list + freelist_index(bytes_left);

                ((Obj*)start_free) -> free_list_next = *__my_free_list;
                *__my_free_list = (Obj*)start_free;
            }
            start_free = (char*)malloc(__bytes_to_get);
            if (nullptr == start_free) {
                size_t __i;
                Obj* volatile* __my_free_list;
            Obj* __p;
                // Try to make do with what we have.  That can't
                // hurt.  We do not try smaller requests, since that tends
                // to result in disaster on multi-process machines.
                for (__i = size;
                    __i <= (size_t) MAX_BYTES;
                    __i += (size_t) ALIGN) {
                    __my_free_list = free_list + freelist_index(__i);
                    __p = *__my_free_list;
                    if (0 != __p) {
                        *__my_free_list = __p -> free_list_next;
                        start_free = (char*)__p;
                        end_free = start_free + __i;
                        return(chunk_alloc(size, nobjs));
                        // Any leftover piece will eventually make it to the
                        // right free list.
                    }
                }
            end_free = 0;	// In case of exception.
                start_free = (char*)malloc_alloc::allocate(__bytes_to_get);
                // This should either throw an
                // exception or remedy the situation.  Thus we assume it
                // succeeded.
            }
            heap_size += __bytes_to_get;
            end_free = start_free + __bytes_to_get;
            return(chunk_alloc(size, nobjs));
        }
    }

};





//类静态成员变量初始化
template <typename T>
char* MyAllocator<T>::start_free = nullptr;

template <typename T>
char* MyAllocator<T>::end_free = nullptr;

template <typename T>
size_t MyAllocator<T>::heap_size = 0;


template <typename T>
typename MyAllocator<T>::Obj* volatile MyAllocator<T>::free_list[NFREELISTS] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};


template <typename T>
std::mutex MyAllocator<T>::mtx;



#endif // MY_ALLOCATOR_H