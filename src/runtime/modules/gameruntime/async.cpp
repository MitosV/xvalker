#include "runtime/modules/gameruntime/gameruntime.hpp"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

// XAsync: one asynchronous operation, tracked in a block the title owns. Whoever implements
// the operation is its provider, a callback asked to begin, do the work, hand out the result
// and clean up. Providers are the title's own libraries as often as they are ours. The work
// runs on the work port of the block's queue and the title's completion callback on its
// completion port.
namespace xv::runtime::modules::gameruntime {
namespace {
enum class Op : std::uint32_t { begin, do_work, get_result, cancel, cleanup };
struct ProviderData {
    XAsyncBlock *block;
    std::size_t buffer_size;
    void *buffer;
    void *context;
};
using Provider = HRESULT CALLBACK(Op op, const ProviderData *data);

// What a block's `internal` words point at while the operation is alive. Once the provider
// has cleaned up, the first word is null and the second holds the final status.
struct State {
    std::atomic<long> references{1};
    XAsyncBlock *block = nullptr;
    Provider *provider = nullptr;
    void *context = nullptr;
    const void *identity = nullptr;
    Queue *queue = nullptr;
    std::vector<std::uint8_t> allocated; // the context of XAsyncBeginAlloc
    std::mutex mutex;
    std::condition_variable changed;
    HRESULT status = e_pending;
    std::size_t result_size = 0;
    bool finishing = false, completed = false, cleaned = false;
};
std::mutex blocks; // guards the `internal` words of every block
// The operations there are. A title copies a block, or completes one whose operation is long
// over: what such a block names is no operation any more.
std::set<const State *> live;

void release(State *state) {
    if (--state->references == 0) {
        {
            const std::lock_guard lock(blocks);
            live.erase(state);
        }
        if (state->queue)
            close(state->queue);
        delete state;
    }
}
State *acquire(XAsyncBlock *block) {
    const std::lock_guard lock(blocks);
    auto *const state = static_cast<State *>(block->internal[0]);
    if (!state || !live.contains(state))
        return nullptr;
    ++state->references;
    return state;
}
HRESULT final_status(XAsyncBlock *block) {
    const std::lock_guard lock(blocks);
    return static_cast<HRESULT>(reinterpret_cast<std::uintptr_t>(block->internal[1]));
}
ProviderData data_of(State *state, std::size_t size = 0, void *buffer = nullptr) {
    return {state->block, size, buffer, state->context};
}
// the provider is done with the operation: the block keeps only its status
void cleanup(State *state) {
    {
        const std::lock_guard lock(state->mutex);
        if (state->cleaned)
            return;
        state->cleaned = true;
    }
    const auto data = data_of(state);
    state->provider(Op::cleanup, &data);
    {
        const std::lock_guard lock(blocks);
        if (state->block->internal[0] == state) {
            state->block->internal[0] = nullptr;
            state->block->internal[1] =
                reinterpret_cast<void *>(static_cast<std::uintptr_t>(state->status));
        }
    }
    release(state);
}
void CALLBACK completed(void *context, bool) {
    auto *const state = static_cast<State *>(context);
    auto *const block = state->block;
    const bool payload = state->result_size != 0;
    // without a payload nothing will ask for a result, and the title's callback may free
    // the block: the provider cleans up first
    if (!payload)
        cleanup(state);
    if (block->callback)
        block->callback(block);
    release(state);
}
void complete(State *state, HRESULT result, std::size_t size) {
    if (result == e_pending)
        return;
    // A block without a callback is the title's to let go of the moment it sees the
    // operation done (XAsyncGetStatus with wait, on a block of its stack): nothing reads or
    // writes it after that.
    const bool notified = state->block->callback != nullptr;
    {
        const std::lock_guard lock(state->mutex);
        if (state->finishing)
            return;
        state->finishing = true;
        state->status = result;
        state->result_size = SUCCEEDED(result) ? size : 0;
    }
    if (!notified && !state->result_size)
        cleanup(state);
    {
        const std::lock_guard lock(state->mutex);
        state->completed = true;
    }
    state->changed.notify_all();
    if (!notified)
        return;
    ++state->references;
    if (FAILED(submit(state->queue, Port::completion, 0, state, &completed)))
        completed(state, true);
}
void CALLBACK work(void *context, bool canceled) {
    auto *const state = static_cast<State *>(context);
    if (canceled) {
        complete(state, E_ABORT, 0);
    } else {
        const auto data = data_of(state);
        // a provider that goes on working answers E_PENDING; one that fails has not completed
        if (const auto result = state->provider(Op::do_work, &data);
            FAILED(result) && result != e_pending)
            complete(state, result, 0);
    }
    release(state);
}

HRESULT begin(XAsyncBlock *block, void *context, const void *identity, Provider *provider,
              std::size_t allocate = 0, const void *parameters = nullptr,
              std::size_t parameters_size = 0) {
    if (!block || !provider)
        return E_INVALIDARG;
    auto *const state = new State;
    {
        const std::lock_guard lock(blocks);
        live.insert(state);
    }
    state->block = block;
    state->provider = provider;
    state->identity = identity;
    state->queue = block->queue ? duplicate(block->queue) : process_queue();
    if (allocate) {
        state->allocated.resize(allocate);
        if (parameters)
            std::memcpy(state->allocated.data(), parameters, std::min(allocate, parameters_size));
        context = state->allocated.data();
    }
    state->context = context;
    {
        const std::lock_guard lock(blocks);
        block->internal[0] = state;
        block->internal[1] = reinterpret_cast<void *>(static_cast<std::uintptr_t>(e_pending));
    }
    ++state->references;
    const auto data = data_of(state);
    if (const auto result = provider(Op::begin, &data); FAILED(result))
        complete(state, result, 0);
    release(state);
    return S_OK;
}

HRESULT WINAPI async_get_status(void *, XAsyncBlock *block, bool wait) {
    if (!block)
        return E_INVALIDARG;
    auto *const state = acquire(block);
    if (!state)
        return final_status(block);
    HRESULT status;
    {
        std::unique_lock lock(state->mutex);
        if (wait)
            state->changed.wait(lock, [&] { return state->completed; });
        status = state->completed ? state->status : e_pending;
    }
    release(state);
    return status;
}
HRESULT WINAPI async_get_result_size(void *, XAsyncBlock *block, std::size_t *size) {
    return result_size(block, size);
}
void WINAPI async_cancel(void *, XAsyncBlock *block) {
    auto *const state = block ? acquire(block) : nullptr;
    if (!state)
        return;
    bool running;
    {
        const std::lock_guard lock(state->mutex);
        running = !state->completed;
    }
    if (running) {
        const auto data = data_of(state);
        state->provider(Op::cancel, &data);
    }
    release(state);
}
// XAsyncRun: the title's function is the work, and its return value the result
HRESULT CALLBACK run_provider(Op op, const ProviderData *data) {
    using Work = HRESULT CALLBACK(XAsyncBlock *);
    if (op == Op::do_work) {
        const auto result = reinterpret_cast<Work *>(data->context)(data->block);
        if (auto *const state = acquire(data->block)) {
            complete(state, result, 0);
            release(state);
        }
    }
    return S_OK;
}
HRESULT WINAPI async_schedule(void *, XAsyncBlock *block, std::uint32_t delay);
HRESULT WINAPI async_run(void *, XAsyncBlock *block, void *function) {
    if (!function)
        return E_INVALIDARG;
    const auto result = begin(block, function, nullptr, &run_provider);
    return FAILED(result) ? result : async_schedule(nullptr, block, 0);
}
HRESULT WINAPI async_begin(void *, XAsyncBlock *block, void *context, const void *identity,
                           const char *, Provider *provider) {
    return begin(block, context, identity, provider);
}
HRESULT WINAPI async_begin_alloc(void *, XAsyncBlock *block, const void *identity, const char *,
                                 Provider *provider, std::size_t context_size,
                                 std::size_t parameters_size, const void *parameters) {
    return begin(block, nullptr, identity, provider, std::max<std::size_t>(context_size, 1),
                 parameters, parameters_size);
}
HRESULT WINAPI async_schedule(void *, XAsyncBlock *block, std::uint32_t delay) {
    auto *const state = block ? acquire(block) : nullptr;
    if (!state)
        return E_INVALIDARG;
    const auto result = submit(state->queue, Port::work, delay, state, &work);
    if (FAILED(result))
        release(state);
    return result;
}
void WINAPI async_complete(void *, XAsyncBlock *block, HRESULT result, std::size_t size) {
    auto *const state = block ? acquire(block) : nullptr;
    if (!state)
        return;
    complete(state, result, size);
    release(state);
}
HRESULT WINAPI async_get_result(void *, XAsyncBlock *block, const void *identity, std::size_t size,
                                void *buffer, std::size_t *used) {
    if (used)
        *used = 0;
    if (!block)
        return E_INVALIDARG;
    auto *const state = acquire(block);
    if (!state)
        return final_status(block);
    HRESULT status;
    std::size_t needed;
    {
        const std::lock_guard lock(state->mutex);
        status = state->completed ? state->status : e_pending;
        needed = state->result_size;
    }
    if (SUCCEEDED(status) && state->identity != identity)
        status = E_INVALIDARG;
    else if (SUCCEEDED(status) && needed) {
        if (size < needed || !buffer) {
            status = e_not_sufficient_buffer;
        } else {
            const auto data = data_of(state, size, buffer);
            status = state->provider(Op::get_result, &data);
            if (SUCCEEDED(status) && used)
                *used = needed;
        }
    }
    // the result can be taken once
    if (status != e_pending && status != e_not_sufficient_buffer && status != E_INVALIDARG)
        cleanup(state);
    release(state);
    return status;
}

// An operation of ours: its outcome is known when it starts
struct Finished {
    HRESULT status;
    std::vector<std::uint8_t> payload;
};
HRESULT CALLBACK finished_provider(Op op, const ProviderData *data) {
    auto *const finished = static_cast<Finished *>(data->context);
    switch (op) {
    case Op::begin:
        return async_schedule(nullptr, data->block, 0);
    case Op::do_work:
        async_complete(nullptr, data->block, finished->status, finished->payload.size());
        return S_OK;
    case Op::get_result:
        std::memcpy(data->buffer, finished->payload.data(), finished->payload.size());
        return S_OK;
    case Op::cancel:
        return S_OK;
    case Op::cleanup:
        delete finished;
        return S_OK;
    }
    return S_OK;
}
// An operation of ours whose outcome another thread brings
struct Deferred {
    Finished finished;
    std::function<void(Outcome)> work;
};
HRESULT CALLBACK deferred_provider(Op op, const ProviderData *data) {
    auto *const deferred = static_cast<Deferred *>(data->context);
    switch (op) {
    case Op::begin:
        return async_schedule(nullptr, data->block, 0);
    case Op::do_work:
        std::exchange(deferred->work, {})(
            [deferred, block = data->block](HRESULT status, std::vector<std::uint8_t> payload) {
                deferred->finished = {status, std::move(payload)};
                async_complete(nullptr, block, status, deferred->finished.payload.size());
            });
        return e_pending;
    case Op::get_result:
        std::memcpy(data->buffer, deferred->finished.payload.data(),
                    deferred->finished.payload.size());
        return S_OK;
    case Op::cancel:
        return S_OK;
    case Op::cleanup:
        delete deferred;
        return S_OK;
    }
    return S_OK;
}
} // namespace

HRESULT start(XAsyncBlock *block, const void *identity, std::function<void(Outcome)> work) {
    if (!block)
        return E_INVALIDARG;
    return begin(block, new Deferred{{e_pending, {}}, std::move(work)}, identity,
                 &deferred_provider);
}
HRESULT finish(XAsyncBlock *block, const void *identity, HRESULT status,
               std::vector<std::uint8_t> payload) {
    if (!block)
        return E_INVALIDARG;
    auto *const finished = new Finished{status, std::move(payload)};
    return begin(block, finished, identity, &finished_provider);
}
HRESULT result(XAsyncBlock *block, const void *identity, std::size_t size, void *buffer,
               std::size_t *used) {
    return async_get_result(nullptr, block, identity, size, buffer, used);
}
HRESULT result_size(XAsyncBlock *block, std::size_t *size) {
    if (!block || !size)
        return E_INVALIDARG;
    *size = 0;
    auto *const state = acquire(block);
    if (!state)
        return final_status(block);
    HRESULT status;
    {
        const std::lock_guard lock(state->mutex);
        status = state->completed ? state->status : e_pending;
        *size = state->result_size;
    }
    release(state);
    return status;
}

void install_async() {
    implement("XAsyncGetStatus", &async_get_status);
    implement("XAsyncGetResultSize", &async_get_result_size);
    implement("XAsyncCancel", &async_cancel);
    implement("XAsyncRun", &async_run);
    implement("XAsyncBegin", &async_begin);
    implement("XAsyncBeginAlloc", &async_begin_alloc);
    implement("XAsyncSchedule", &async_schedule);
    implement("XAsyncComplete", &async_complete);
    implement("XAsyncGetResult", &async_get_result);
}
} // namespace xv::runtime::modules::gameruntime
