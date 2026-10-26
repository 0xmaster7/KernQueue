# Design and review notes

## Lifetime

`open` allocates a private context; `file->private_data` owns it until final `release`. In-flight syscalls retain file references, so release cannot race those operations. `.owner = THIS_MODULE` pins module code while files remain open. Release invokes `cancel_work_sync` before reclaiming either list and the context. Initialization unwinds workqueue allocation if device registration fails.

## State invariant

`outstanding = pending + executing + completed`, bounded by 64 accepted jobs per context. `completed` equals the done-list length. All writes to counters/lists hold the context mutex. Readiness conditions use `READ_ONCE` and are checked again under the mutex before changing state. Wakeups follow completion publication or capacity release. Waiters register before rechecking their condition through `wait_event_interruptible`/`poll_wait`, avoiding a check-then-sleep lost wakeup.

Only one work item processes a context, preserving FIFO completion order. Different opens can run on separate workers. Work is queued while the writer holds the mutex, so the worker always observes the newly published job. The workqueue supports requeueing an executing item; a write racing the worker's empty-list exit is either consumed by the current run or triggers another run.

## User memory

Requests are copied into an allocated job before queue admission. Invalid fields and failed copies free that job. Responses are zero-initialized by `kzalloc`, including reserved output bytes. Reads copy under the mutex and remove the result only after a successful copy. A failed copy can partially modify the caller's buffer, but cannot consume a result.

One temporary allocation exists per blocked writer in addition to accepted jobs. Together with unlimited opens, that means the queue capacity is not a global memory bound. The root-only interface limits access; adding global accounting would be needed before broader deployment.

## Evaluation

The client has an independent userspace FNV-1a implementation and a fixed known vector. It verifies all payload lengths, FIFO order, invalid requests, bad pointers, readiness, backpressure, signal interruption, per-open isolation, shared-descriptor concurrency, and close with outstanding work. The guest also unloads/reloads the module five times and checks for kernel warning/oops signatures. These checks are evidence for the exercised cases, not a general absence-of-races proof.
