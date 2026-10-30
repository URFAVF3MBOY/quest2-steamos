/* q2msm shim: 5.4 renamed reservation_object -> dma_resv.  Map the names the 5.10 driver uses onto 4.19's API. */
#ifndef _Q2MSM_DMA_RESV_H
#define _Q2MSM_DMA_RESV_H
#include <linux/reservation.h>
#define dma_resv                         reservation_object
#define dma_resv_list                    reservation_object_list
#define dma_resv_init                    reservation_object_init
#define dma_resv_fini                    reservation_object_fini
#define dma_resv_add_excl_fence          reservation_object_add_excl_fence
#define dma_resv_add_shared_fence        reservation_object_add_shared_fence
#define dma_resv_reserve_shared(o, n)    reservation_object_reserve_shared(o)
#define dma_resv_get_list                reservation_object_get_list
#define dma_resv_get_excl                reservation_object_get_excl
#define dma_resv_get_excl_rcu            reservation_object_get_excl_rcu
#define dma_resv_get_fences_rcu          reservation_object_get_fences_rcu
#define dma_resv_wait_timeout_rcu        reservation_object_wait_timeout_rcu
#define dma_resv_test_signaled_rcu       reservation_object_test_signaled_rcu
#define dma_resv_held                    reservation_object_held
#define dma_resv_assert_held             reservation_object_assert_held
#define dma_resv_lock(o, c)                  ww_mutex_lock(&(o)->lock, (c))
#define dma_resv_lock_interruptible(o, c)    ww_mutex_lock_interruptible(&(o)->lock, (c))
#define dma_resv_lock_slow_interruptible(o, c) ww_mutex_lock_slow_interruptible(&(o)->lock, (c))
#define dma_resv_unlock(o)                   ww_mutex_unlock(&(o)->lock)
#endif
