#include"MemoryPool_v1.h"

namespace MemoryPool_v1
{
	// 内存池构造函数，初始化块大小
	MemoryPool::MemoryPool(size_t BlockSize) 
		: BlockSize(BlockSize) {}
	
	// 析构函数，依次释放所有块的内存
	MemoryPool::~MemoryPool()
	{
		Slot* cur = firstBlock;
		while (cur)
		{
			Slot* next = cur->next;
			// operator delete显示释放指向的一块内存，其与delete不同在于，delete会先调用析构函数
			// 这里是与operator new配合使用
			// 因为我们开辟内存池的块就是operator new直接分配一块原始内存，没有需要初始化的东西
			operator delete(reinterpret_cast<void*>(cur));
			cur = next;
		}
	}

	// 内存池初始化槽大小及各指针
	void MemoryPool::Init(size_t SlotSize)
	{
		assert(SlotSize > 0);
		this->SlotSize = SlotSize;
		firstBlock = nullptr;
		curSlot = nullptr;
		freeList = nullptr;
		lastSlot = nullptr;
	}

	// 分配一个空闲槽，返回该空闲槽的地址
	void* MemoryPool::allocate()
	{
		// 先查看空闲槽列表，如果非空，直接从空闲槽列表中取一个
		if (freeList != nullptr)
		{
			// 加锁，保证多线程下操作原子性  (⭐)
			std::lock_guard<std::mutex> lock(mutexForFreeList);
			// 当前线程获取锁后，需再次确保空闲槽非空
			// 不然，当两个线程都执行到(⭐)处，且只有一个空闲槽时，可能会出问题
			if (freeList != nullptr)
			{
				// 从空闲槽列表头部取出一个空闲槽
				Slot* temp = freeList;
				freeList = freeList->next;
				return temp;
			}
		}

		// 如果空闲槽列表为空，只好从当前块拿空闲的槽
		// (思考：那么为什么要设计空闲槽列表呢？)
		// (答：如果没有空闲槽列表，那些在curSlot之前申请的槽，被释放后就无法管理了。你不能再将curSlot往回移，最好的办法就是用另一个指针（即空闲槽列表）去管理)
		Slot* temp;
		std::lock_guard<std::mutex> lock(mutexForBlock);
		if (curSlot >= lastSlot)  // 如果当前块没有可用的槽了，只好申请开辟新的块
		{
			allocateNewBlock();
		}
		temp = curSlot;
		curSlot += SlotSize / sizeof(Slot);
		return temp;
	}

	// 回收一个空闲槽
	void MemoryPool::deallocate(void* ptr)
	{
		if (ptr)
		{
			std::lock_guard<std::mutex> lock(mutexForFreeList);
			// 依旧头插法
			reinterpret_cast<Slot*>(ptr)->next = freeList;
			freeList = reinterpret_cast<Slot*>(ptr);
		}
	}

	// 内存池当前块都已用尽，申请增加新的块
	void MemoryPool::allocateNewBlock()
	{
		// 直接operator new一块原始内存
		void* newBlock = operator new(BlockSize);
		// 头插法，新申请的块成为当前内存池的第一个块
		reinterpret_cast<Slot*>(newBlock)->next = firstBlock;
		firstBlock = reinterpret_cast<Slot*>(newBlock);

		char* body = reinterpret_cast<char*>(newBlock) + sizeof(Slot*);// skip next pointer
		size_t paddingSize = padPointer(body, SlotSize);
		curSlot = reinterpret_cast<Slot*>(body + paddingSize);

		// 这里加1配合之前curSlot>=lastSlot，比较巧妙，可以画图想象一下
		lastSlot = reinterpret_cast<Slot*>(reinterpret_cast<size_t>(newBlock) + BlockSize - SlotSize + 1);
		
		freeList = nullptr;
	}

	size_t MemoryPool::padPointer(char* p, size_t align)
	{
		size_t remain = reinterpret_cast<size_t>(p) % align;
		return (remain == 0) ? 0 : (align - remain);
	}

	//////////////////////////////////////////
	// HashBucket类实现

	void HashBucket::initMemoryPool()
	{
		for (int i = 0; i < MEMORY_POOL_NUM; i++)
		{
			getMemoryPool(i).Init((i + 1) * SLOT_BASE_SIZE);
		}
	}

	// single instance
	// 不是指 HashBucket 是单例，而是它有一个静态成员变量
	MemoryPool& HashBucket::getMemoryPool(int index)
	{
		static MemoryPool memoryPool[MEMORY_POOL_NUM];
		return memoryPool[index];
	}

	// 根据待申请内存大小，找到HashBucket对应的桶
	void* HashBucket::useMemory(size_t size)
	{
		// 申请内存要大于0
		if (size <= 0)
			return nullptr;
		// 如果申请的内存超过最大槽的大小，那就不用内存池了，直接用系统调用就好
		// 解释：因为内存池主要解决的是 小内存的内存碎片问题 和 小内存频繁申请释放的性能问题！
		if (size > MAX_SLOT_SIZE)
			return operator new(size);
		// 找对应槽时向上取整，保证槽大小能容纳申请的内存
		return getMemoryPool((size + SLOT_BASE_SIZE - 1) / SLOT_BASE_SIZE - 1).allocate();
	}

	void HashBucket::freeMemory(void* ptr, size_t size)
	{
		if (!ptr)
			return;
		if (size > MAX_SLOT_SIZE)
		{
			operator delete(ptr);
			return;
		}
		getMemoryPool((size + SLOT_BASE_SIZE - 1) / SLOT_BASE_SIZE - 1).deallocate(ptr);
	}
}