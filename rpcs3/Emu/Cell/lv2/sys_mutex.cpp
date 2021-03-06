#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "sys_mutex.h"

namespace vm { using namespace ps3; }

logs::channel sys_mutex("sys_mutex", logs::level::notice);

extern u64 get_system_time();

void lv2_mutex::unlock(lv2_lock_t)
{
	owner.reset();

	if (sq.size())
	{
		// pick new owner; protocol is ignored in current implementation
		owner = idm::get<ppu_thread>(sq.front()->id);
		owner->set_signal();
	}
}

s32 sys_mutex_create(vm::ptr<u32> mutex_id, vm::ptr<sys_mutex_attribute_t> attr)
{
	sys_mutex.warning("sys_mutex_create(mutex_id=*0x%x, attr=*0x%x)", mutex_id, attr);

	if (!mutex_id || !attr)
	{
		return CELL_EFAULT;
	}

	const u32 protocol = attr->protocol;

	switch (protocol)
	{
	case SYS_SYNC_FIFO: break;
	case SYS_SYNC_PRIORITY: break;
	case SYS_SYNC_PRIORITY_INHERIT: break;

	default:
	{
		sys_mutex.error("sys_mutex_create(): unknown protocol (0x%x)", protocol);
		return CELL_EINVAL;
	}
	}

	const bool recursive = attr->recursive == SYS_SYNC_RECURSIVE;

	if ((!recursive && attr->recursive != SYS_SYNC_NOT_RECURSIVE) || attr->pshared != SYS_SYNC_NOT_PROCESS_SHARED || attr->adaptive != SYS_SYNC_NOT_ADAPTIVE || attr->ipc_key || attr->flags)
	{
		sys_mutex.error("sys_mutex_create(): unknown attributes (recursive=0x%x, pshared=0x%x, adaptive=0x%x, ipc_key=0x%llx, flags=0x%x)", attr->recursive, attr->pshared, attr->adaptive, attr->ipc_key, attr->flags);

		return CELL_EINVAL;
	}

	*mutex_id = idm::make<lv2_obj, lv2_mutex>(recursive, protocol, attr->name_u64);

	return CELL_OK;
}

s32 sys_mutex_destroy(u32 mutex_id)
{
	sys_mutex.warning("sys_mutex_destroy(mutex_id=0x%x)", mutex_id);

	LV2_LOCK;

	const auto mutex = idm::get<lv2_obj, lv2_mutex>(mutex_id);

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	if (mutex->owner || mutex->sq.size())
	{
		return CELL_EBUSY;
	}

	if (mutex->cond_count)
	{
		return CELL_EPERM;
	}

	idm::remove<lv2_obj, lv2_mutex>(mutex_id);

	return CELL_OK;
}

s32 sys_mutex_lock(ppu_thread& ppu, u32 mutex_id, u64 timeout)
{
	sys_mutex.trace("sys_mutex_lock(mutex_id=0x%x, timeout=0x%llx)", mutex_id, timeout);

	const u64 start_time = get_system_time();

	LV2_LOCK;

	const auto mutex = idm::get<lv2_obj, lv2_mutex>(mutex_id);

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	// check current ownership
	if (mutex->owner.get() == &ppu)
	{
		if (mutex->recursive)
		{
			if (mutex->recursive_count == 0xffffffffu)
			{
				return CELL_EKRESOURCE;
			}

			mutex->recursive_count++;

			return CELL_OK;
		}

		return CELL_EDEADLK;
	}

	// lock immediately if not locked
	if (!mutex->owner)
	{
		mutex->owner = idm::get<ppu_thread>(ppu.id);

		return CELL_OK;
	}

	// add waiter; protocol is ignored in current implementation
	sleep_entry<cpu_thread> waiter(mutex->sq, ppu);

	while (!ppu.state.test_and_reset(cpu_flag::signal))
	{
		CHECK_EMU_STATUS;

		if (timeout)
		{
			const u64 passed = get_system_time() - start_time;

			if (passed >= timeout)
			{
				return CELL_ETIMEDOUT;
			}

			LV2_UNLOCK, thread_ctrl::wait_for(timeout - passed);
		}
		else
		{
			LV2_UNLOCK, thread_ctrl::wait();
		}
	}

	// new owner must be set when unlocked
	if (mutex->owner.get() != &ppu)
	{
		fmt::throw_exception("Unexpected mutex owner" HERE);
	}

	return CELL_OK;
}

s32 sys_mutex_trylock(ppu_thread& ppu, u32 mutex_id)
{
	sys_mutex.trace("sys_mutex_trylock(mutex_id=0x%x)", mutex_id);

	LV2_LOCK;

	const auto mutex = idm::get<lv2_obj, lv2_mutex>(mutex_id);

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	// check current ownership
	if (mutex->owner.get() == &ppu)
	{
		if (mutex->recursive)
		{
			if (mutex->recursive_count == 0xffffffffu)
			{
				return CELL_EKRESOURCE;
			}

			mutex->recursive_count++;

			return CELL_OK;
		}

		return CELL_EDEADLK;
	}

	if (mutex->owner)
	{
		return CELL_EBUSY;
	}

	// own the mutex if free
	mutex->owner = idm::get<ppu_thread>(ppu.id);

	return CELL_OK;
}

s32 sys_mutex_unlock(ppu_thread& ppu, u32 mutex_id)
{
	sys_mutex.trace("sys_mutex_unlock(mutex_id=0x%x)", mutex_id);

	LV2_LOCK;

	const auto mutex = idm::get<lv2_obj, lv2_mutex>(mutex_id);

	if (!mutex)
	{
		return CELL_ESRCH;
	}

	// check current ownership
	if (mutex->owner.get() != &ppu)
	{
		return CELL_EPERM;
	}

	if (mutex->recursive_count)
	{
		if (!mutex->recursive)
		{
			fmt::throw_exception("Unexpected recursive_count" HERE);
		}
		
		mutex->recursive_count--;
	}
	else
	{
		mutex->unlock(lv2_lock);
	}

	return CELL_OK;
}
