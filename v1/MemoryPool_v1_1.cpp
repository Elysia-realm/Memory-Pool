#include"MemoryPool_v1_1.h"

namespace MemoryPool_v1_1
{
	MemoryPool::MemoryPool(size_t BlockSize)
		: BlockSize(BlockSize) {}

	MemoryPool::~MemoryPool()
	{
		Slot* cur = firstBlock;
		while (cur)
		{
			Slot* next = cur->next;
			operator delete(reinterpret_cast<void*>(cur));
			cur = next;
		}
	}

	// 只有Init、allocate、deallocate函数改动
	void MemoryPool::Init(size_t SlotSize)
	{
		assert(SlotSize > 0);
		this->SlotSize = SlotSize;
		firstBlock = nullptr;
		curSlot = nullptr;
		// freeList = nullptr;
		freeList.store(nullptr, std::memory_order_relaxed);	 // relaxed只保证原子性，不施加同步或顺序约束
		lastSlot = nullptr;
	}

	void* MemoryPool::allocate()
	{
		/*
		if (freeList != nullptr)
		{
			std::lock_guard<std::mutex> lock(mutexForFreeList);
			if (freeList != nullptr)
			{
				Slot* temp = freeList;
				freeList = freeList->next;
				return temp;
			}
		}
		*/
		Slot* slot = popFreeList();	  // 优先从空闲列表取，只不过通过无锁方法
		if (slot != nullptr)
			return slot;

		Slot* temp;
		std::lock_guard<std::mutex> lock(mutexForBlock);
		if (curSlot >= lastSlot)
		{
			allocateNewBlock();
		}
		temp = curSlot;
		curSlot += SlotSize / sizeof(Slot);
		return temp;
	}

	void MemoryPool::deallocate(void* ptr)
	{
		/*
		if (ptr)
		{
			std::lock_guard<std::mutex> lock(mutexForFreeList);
			reinterpret_cast<Slot*>(ptr)->next = freeList;
			freeList = reinterpret_cast<Slot*>(ptr);
		}
		*/
		if (!ptr) return;
		Slot* slot = reinterpret_cast<Slot*>(ptr);
		pushFreeList(slot);  // 回收使用后的槽，同样是无锁方法
	}

	void MemoryPool::allocateNewBlock()
	{
		void* newBlock = operator new(BlockSize);
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

	// "无锁"地对空闲列表头插
	bool MemoryPool::pushFreeList(Slot* slot)
	{
		// 无锁算法常用的重试循环，“比较-交换-重试”
		while (true)
		{
			// 读空闲列表头指针. 为什么不用内存顺序约束呢？（要配合下面CAS理解）
			Slot* oldHead = freeList.load(std::memory_order_relaxed);
			// 头插，要加入列表的节点的next指向旧的头指针
			slot->next.store(oldHead, std::memory_order_relaxed);
			// CAS，比较-交换原子操作. this->compare_exchange(expected, desired)
			// 如果当前值==expected, 则当前值修改为desired; 如果不等于，则将当前值写回expected（体会这句话，不是将expected再赋给当前值）
			if (freeList.compare_exchange_weak(oldHead, slot, std::memory_order_release, std::memory_order_relaxed))
			{
				// 如果当前空闲列表的头指针，就是之前的oldHead，说明没有别的线程抢先一步push过，则当前线程可完全将slot赋给头指针，return true
				// 如果当前的freelist不是之前的oldhead，说明有别的线程抢先修改过，那么本线程的比较-交换失败，必须重试一遍，继续while循环
				return true;
				// 其中，如果交换成功return true，我们使用release内存顺序。
				// 这是为了确保后面的线程能看到当前线程的这次写入结果
			}
		}
	}

	// “无锁”地从空闲列表头部取一个槽节点
	Slot* MemoryPool::popFreeList()
	{
		while (true)
		{
			// 简单记忆：release用于写共享数据后，acquire用于读共享数据前

			// acquire，确保当前线程看到之前线程的写入结果
			Slot* oldHead = freeList.load(std::memory_order_acquire);
			if (oldHead == nullptr)
				return nullptr;

			Slot* newHead = nullptr;
			try {
				newHead = oldHead->next.load(std::memory_order_relaxed);
			}
			catch(...){
				continue;
			}

			if (freeList.compare_exchange_weak(oldHead, newHead, std::memory_order_acquire, std::memory_order_relaxed))
			{
				return oldHead;
			}
		}
	}

	void HashBucket::initMemoryPool()
	{
		for (int i = 0; i < MEMORY_POOL_NUM; i++)
		{
			getMemoryPool(i).Init((i + 1) * SLOT_BASE_SIZE);
		}
	}

	// single instance
	MemoryPool& HashBucket::getMemoryPool(int index)
	{
		static MemoryPool memoryPool[MEMORY_POOL_NUM];
		return memoryPool[index];
	}

	void* HashBucket::useMemory(size_t size)
	{
		if (size <= 0)
			return nullptr;
		if (size > MAX_SLOT_SIZE)
			return operator new(size);
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