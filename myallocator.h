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
        size_t bytes_left = end_free - start_free;//内存池剩余空间

        if (bytes_left >= total_bytes) {
            result = start_free;
            start_free += total_bytes;
            return(result);
        } else if (bytes_left >= size) {
            //内存池剩余空间不能完全满足需求量但足够供应一个（含）以上的区块
            nobjs = (int)(bytes_left / size);
            total_bytes = size * nobjs;
            result = start_free;
            start_free += total_bytes;
            return(result);
        } else {
            //内存池剩余空间连一个区块的大小都无法提供
            size_t bytes_to_get = 2 * total_bytes + round_up(heap_size >> 4);
            //试着让内存池的残余零头还有利用价值
            if (bytes_left > 0) {
                //内存池还有一些零头，先配给适当的free List
                //首先寻找适当的free List
                Obj* volatile* my_free_list =
                        free_list + freelist_index(bytes_left);
                //调整free List，将内存池中的残余空间编入
                ((Obj*)start_free) -> free_list_next = *my_free_list;
                *my_free_list = (Obj*)start_free;
            }
            start_free = (char*)malloc(bytes_to_get);
            if (nullptr == start_free) {
                //heap 空间不足，malloc()失败
                size_t i;
                Obj* volatile* my_free_list;
            Obj* p;
                //试看检视我们手上拥有的东西。这不会造成伤善，我们不打算会试配直
                //较小的区块，因为那再多进程机器上容易导致灾难
                //以下搜寻适当的free 1ist
                //所谓适当是指”尚有未用区块，且区块够大"的free List

                for (i = size; i <= (size_t) MAX_BYTES; i += (size_t) ALIGN) {
                    my_free_list = free_list + freelist_index(i);
                    p = *my_free_list;
                    if (0 != p) {
                        *my_free_list = p -> free_list_next;
                        start_free = (char*)p;
                        end_free = start_free + i;
                        return(chunk_alloc(size, nobjs));
                        //注意，任何残余零头终将被编入释放的free List中备用
                    }
                }
            end_free = 0;	//如果出现意外（山穷水尽，到处都没内存可用了）//如果出现意外（山穷水尽，到处都没内存可用了）
                //调用第一级配置器，看看out-of-memory机制能够尽点力
                start_free = (char*)malloc_alloc::allocate(bytes_to_get);
                //这会导致抛出异常，或内存不足的情况获得改善//这会导致抛出异常，或内存不足的情况获得改善
            }
            heap_size += bytes_to_get;
            end_free = start_free + bytes_to_get;
            //递归调用自己，为了修正nobjs
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