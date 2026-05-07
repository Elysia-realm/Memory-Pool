#pragma once
#include<mutex>
#include<cassert>

// 定义内存池元数据
#define MEMORY_POOL_NUM 64		// 内存池数量
#define SLOT_BASE_SIZE 8		// 数据槽基础大小
#define MAX_SLOT_SIZE 512		// 最大数据槽大小

/*
	MemoryPool_v1 实现了基于哈希映射的多种定长内存分配器
	哈希表的每一项对应一个内存池，每个内存池有许多大小相同的块组成（BlockSize = 4096字节）
	但不同内存池的块里被细分为不同大小的槽（SlotSize = 8, 16, 24, 32, 40, ..., 512）
	当用户申请内存时，首先根据需求空间大小找到对应内存池，然后从分配一个空闲的数据槽
*/
namespace MemoryPool_v1
{
	// 内存槽
	struct Slot
	{
		Slot* next;	 // 指向下一个槽
	};

	// 内存池类
	class MemoryPool
	{
	public:
		MemoryPool(size_t BlockSize = 4096);
		~MemoryPool();

		void Init(size_t);
		void* allocate();
		void deallocate(void*);
	private:
		void allocateNewBlock();
		size_t padPointer(char* p, size_t align);

		int BlockSize;							      // 内存池各个块的大小
		int SlotSize;							        // 块中槽的大小
		Slot* firstBlock;						      // 指向内存池管理的第一个块
		Slot* curSlot;							      // 指向当前未被使用的一个槽
		Slot* freeList;							      // 空闲槽列表（使用后又被释放）
		Slot* lastSlot;							      // 内存池最后一个能存放元素的槽（超过该位置，该内存已满，需申请新内存池）
		std::mutex mutexForFreeList;			// 用于空闲槽列表在多线程下增删元素的锁
		std::mutex mutexForBlock;				  // 避免多线程下，重复开辟新内存块造成浪费
	};

	// 哈希表类，每个桶对应的内存池：块大小都相同，但槽大小不同
	class HashBucket
	{
	public:
		static void initMemoryPool();	// 初始化所有内存池，在使用内存池前调用
		static MemoryPool& getMemoryPool(int index);

		static void* useMemory(size_t size);
		static void freeMemory(void* ptr, size_t size);

		// 暴露new, delete两个友元函数作为使用内存池的接口
		template<typename T, typename... Args>
		friend T* newElement(Args&&... args); // Args是函数模板的参数包列表
											                    // 此处使用万能引用，目的是配合完美转发，保持参数的左值/右值属性不变
		template<typename T>
		friend void deleteElement(T* p);
	};

	// 必须在类外声明一次，且函数实现也要与定义写在一起
	// (因为是模板友元函数，参数包列表没有类类型，无法ADL(参数依赖注入))
	template<typename T, typename... Args>
	T* newElement(Args&&... args)
	{
		T* p = nullptr;
		// 根据变量大小，找到槽大小足够的内存池，分配一个空闲槽
		if ((p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)))) != nullptr)
		{
			// 在预先分配的内存地址（槽）上，构造对应类型的变量
			// 这种只构造不分配的new，叫做placement new
			new(p) T(std::forward<Args>(args)...);
		}
		return p;
	}

	template<typename T>
	void deleteElement(T* p)
	{
		if (p)
		{
			// 由于p是经由placement new构造的，必须手动析构
			p->~T(); // 如果是非类类型，那么这是一个伪析构函数（符合语法，但不会执行任何操作）
			// 将p使用的槽回收，并加入所属内存池的空闲槽列表
			HashBucket::freeMemory(reinterpret_cast<void*>(p), sizeof(T));
		}
	}
}
