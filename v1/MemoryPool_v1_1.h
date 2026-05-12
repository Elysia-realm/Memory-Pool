#pragma once
#include<mutex>
#include<cassert>
#include<atomic>

#define MEMORY_POOL_NUM 64
#define SLOT_BASE_SIZE 8
#define MAX_SLOT_SIZE 512

/*
	v1_1与v1区别在于，使用了无锁数据结构（高并发优化）
	所谓无锁，是用硬件级别的原子操作来代替锁，进行线程同步
	无锁，能一定程度减少锁、互斥量等阻塞机制带来的调度开销

	(不过目前仅10个线程下测试，无锁反倒没有优势)	fix: 发现之前无锁popFreeList有错(未对newHead赋值)
	(且无锁入队出队可能有bug？)						 现在10线程，无锁没优势，但强于newDelete；但100线程时还是最差
*/
namespace MemoryPool_v1_1
{
	struct Slot
	{
		//Slot* next;
		std::atomic<Slot*> next;	// 使用atomic模板类包裹变量，用于实现对变量的原子操作
									// atomic具备一些成员函数，如load(原子读取值)、store(原子写入值)
									// 同时这些原子操作会配合memory_order参数控制相互间的可见性和排序约束
	};

	class MemoryPool
	{
	public:
		MemoryPool(size_t BlockSize = 4096);
		~MemoryPool();

		// 只有3个函数发生改动
		void Init(size_t);
		void* allocate();
		void deallocate(void*);
	private:
		void allocateNewBlock();
		size_t padPointer(char* p, size_t align);

		// 新增两个函数，实现对空闲列表节点的无锁增加和删除
		bool pushFreeList(Slot* slot);
		Slot* popFreeList();

		int BlockSize;
		int SlotSize;
		Slot* firstBlock;
		Slot* curSlot;
		//Slot* freeList;
		std::atomic<Slot*> freeList;	// 对于空闲列表这一共享数据，我们不再用锁，改用原子操作做线程同步
		Slot* lastSlot;
		//std::mutex mutexForFreeList;
		std::mutex mutexForBlock;		// 对于内存池块仍用锁（为什么？）
	};

	// HashBucket作为上层结构，无需改动
	class HashBucket
	{
	public:
		static void initMemoryPool();
		static MemoryPool& getMemoryPool(int index);

		static void* useMemory(size_t size);
		static void freeMemory(void* ptr, size_t size);

		template<typename T, typename... Args>
		friend T* newElement(Args&&... args);

		template<typename T>
		friend void deleteElement(T* p);
	};

	template<typename T, typename... Args>
	T* newElement(Args&&... args)
	{
		T* p = nullptr;
		if ((p = reinterpret_cast<T*>(HashBucket::useMemory(sizeof(T)))) != nullptr)
		{
			new(p) T(std::forward<Args>(args)...);
		}
		return p;
	}

	template<typename T>
	void deleteElement(T* p)
	{
		if (p)
		{
			p->~T();
			HashBucket::freeMemory(reinterpret_cast<void*>(p), sizeof(T));
		}
	}
}