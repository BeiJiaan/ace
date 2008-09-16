// $Id$

#include "ace/OS_NS_errno.h"
#include "ace/Dev_Poll_Reactor.h"
#include "ace/Signal.h"
#include "ace/Sig_Handler.h"

ACE_RCSID (ace,
           Dev_Poll_Reactor,
           "$Id$")

#if defined (ACE_HAS_EVENT_POLL) || defined (ACE_HAS_DEV_POLL)

# include "ace/OS_NS_unistd.h"
# include "ace/OS_NS_fcntl.h"
# include "ace/OS_NS_stropts.h"

# if defined (ACE_HAS_EVENT_POLL) && defined (linux)
#  include /**/ <sys/epoll.h>
# elif defined (ACE_HAS_DEV_POLL)
#    if defined (linux)
#      include /**/ <linux/devpoll.h>
#    elif defined (HPUX_VERS) && HPUX_VERS < 1123
#      include /**/ <devpoll.h>
#    else
#      include /**/ <sys/devpoll.h>
#    endif  /* linux */
# endif  /* ACE_HAS_DEV_POLL */

#if !defined (__ACE_INLINE__)
# include "ace/Dev_Poll_Reactor.inl"
#endif /* __ACE_INLINE__ */


#include "ace/Handle_Set.h"
#include "ace/Reactor.h"
#include "ace/Timer_Heap.h"
#include "ace/Timer_Queue.h"
#include "ace/ACE.h"
#include "ace/Reverse_Lock_T.h"
#include "ace/Recursive_Thread_Mutex.h"
#include "ace/Null_Mutex.h"
#include "ace/os_include/os_poll.h"
#include "ace/OS_NS_sys_mman.h"
#include "ace/Guard_T.h"
#include "ace/OS_NS_string.h"
#include "ace/OS_NS_sys_time.h"

ACE_BEGIN_VERSIONED_NAMESPACE_DECL

ACE_Dev_Poll_Reactor_Notify::ACE_Dev_Poll_Reactor_Notify (void)
  : dp_reactor_ (0)
  , notification_pipe_ ()
  , max_notify_iterations_ (-1)
#if defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE)
  , notification_queue_ ()
#endif  /* ACE_HAS_REACTOR_NOTIFICATION_QUEUE */
{
}

int
ACE_Dev_Poll_Reactor_Notify::open (ACE_Reactor_Impl *r,
                                   ACE_Timer_Queue * /* timer_queue */,
                                   int disable_notify_pipe)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::open");

  if (disable_notify_pipe == 0)
    {
      this->dp_reactor_ = dynamic_cast<ACE_Dev_Poll_Reactor *> (r);

      if (this->dp_reactor_ == 0)
        {
          errno = EINVAL;
          return -1;
        }

      if (this->notification_pipe_.open () == -1)
        return -1;

#if defined (F_SETFD)
      // close-on-exec
      ACE_OS::fcntl (this->notification_pipe_.read_handle (), F_SETFD, 1);
      ACE_OS::fcntl (this->notification_pipe_.write_handle (), F_SETFD, 1);
#endif /* F_SETFD */

#if defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE)
      if (notification_queue_.open () == -1)
        {
          return -1;
        }

      if (ACE::set_flags (this->notification_pipe_.write_handle (),
                          ACE_NONBLOCK) == -1)
        return -1;
#endif /* ACE_HAS_REACTOR_NOTIFICATION_QUEUE */

      // Set the read handle into non-blocking mode since we need to
      // perform a "speculative" read when determining if there are
      // notifications to dispatch.
      if (ACE::set_flags (this->notification_pipe_.read_handle (),
                          ACE_NONBLOCK) == -1)
        return -1;
    }

  return 0;
}

int
ACE_Dev_Poll_Reactor_Notify::close (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::close");

#if defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE)
  notification_queue_.reset ();
#endif /* ACE_HAS_REACTOR_NOTIFICATION_QUEUE */

  return this->notification_pipe_.close ();
}

int
ACE_Dev_Poll_Reactor_Notify::notify (ACE_Event_Handler *eh,
                                     ACE_Reactor_Mask mask,
                                     ACE_Time_Value *timeout)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::notify");

  // Just consider this method a "no-op" if there's no
  // ACE_Dev_Poll_Reactor configured.
  if (this->dp_reactor_ == 0)
    return 0;

  ACE_Notification_Buffer buffer (eh, mask);

#if defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE)
  ACE_UNUSED_ARG (timeout);
  ACE_Dev_Poll_Handler_Guard eh_guard (eh);

  int notification_required =
    notification_queue_.push_new_notification (buffer);

  if (notification_required == -1)
    return -1;             // Also decrement eh's reference count

  // The notification has been queued, so it will be delivered at some
  // point (and may have been already); release the refcnt guard.
  eh_guard.release ();

  if (notification_required == 0)
    return 0;

  // Now pop the pipe to force the callback for dispatching when ready. If
  // the send fails due to a full pipe, don't fail - assume the already-sent
  // pipe bytes will cause the entire notification queue to be processed.
  ssize_t n = ACE::send (this->notification_pipe_.write_handle (),
                         (char *) &buffer,
                         1,             // Only need one byte to pop the pipe
                         &ACE_Time_Value::zero);
  if (n == -1 && (errno != ETIME && errno != EAGAIN))
    return -1;

  return 0;
#else

  ACE_Dev_Poll_Handler_Guard eh_guard (eh);

  ssize_t n = ACE::send (this->notification_pipe_.write_handle (),
                         (char *) &buffer,
                         sizeof buffer,
                         timeout);
  if (n == -1)
    return -1;

  eh_guard.release ();

  return 0;
#endif /* ACE_HAS_REACTOR_NOTIFICATION_QUEUE */
}

int
ACE_Dev_Poll_Reactor_Notify::dispatch_notifications (
  int & /* number_of_active_handles */,
  ACE_Handle_Set & /* rd_mask */)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::dispatch_notifications");

  // This method is unimplemented in the ACE_Dev_Poll_Reactor.
  // Instead, the notification handler is invoked as part of the IO
  // event set.  Doing so alters the some documented semantics that
  // state that the notifications are handled before IO events.
  // Enforcing such semantics does not appear to be beneficial, and
  // also serves to slow down event dispatching particularly with this
  // ACE_Dev_Poll_Reactor.

  ACE_NOTSUP_RETURN (-1);
}

int
ACE_Dev_Poll_Reactor_Notify::read_notify_pipe (ACE_HANDLE handle,
                                               ACE_Notification_Buffer &buffer)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::read_notify_pipe");

  // This is a (non-blocking) "speculative" read, i.e., we attempt to
  // read even if no event was polled on the read handle.  A
  // speculative read is necessary since notifications must be
  // dispatched before IO events.  We can avoid the speculative read
  // by "walking" the array of pollfd structures returned from
  // `/dev/poll' or `/dev/epoll' but that is potentially much more
  // expensive than simply checking for an EWOULDBLOCK.
  size_t to_read;
  char *read_p;
  bool have_one = false;

#if defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE)
  // For the queued case, we'll try to read one byte (since that's what
  // the notify () tried to put in) but we don't need it - notifications can
  // be queued even if the pipe fills, so there may be more notifications
  // queued than there are bytes in the pipe.
  char b;
  read_p = &b;
  to_read = 1;

  // Before reading the byte, pop a message from the queue and queue a
  // new message unless the queue is now empty.  The protocol is to
  // keep a byte in the pipe as long as the queue is not empty.
  bool more_messages_queued = false;
  ACE_Notification_Buffer next;

  int result = notification_queue_.pop_next_notification (buffer,
                                                          more_messages_queued,
                                                          next);

  if (result == 0)
    {
      // remove the notification byte from the pipe, avoiding notification loop
      ACE::recv (handle, read_p, to_read);
      return 0;
    }

  if (result == -1)
    return -1;

  if (more_messages_queued)
    (void) ACE::send (this->notification_pipe_.write_handle (),
                      (char *)&next,
                      1 /* one byte is enough */,
                      &ACE_Time_Value::zero);
#else
  to_read = sizeof buffer;
  read_p = (char *)&buffer;
#endif /* ACE_HAS_REACTOR_NOTIFICATION_QUEUE */

  ssize_t n = ACE::recv (handle, read_p, to_read);

  if (n > 0)
    {
      // Check to see if we've got a short read.
      if (static_cast<size_t> (n) != to_read)
        {
          size_t remainder = to_read - n;

          // If so, try to recover by reading the remainder.  If this
          // doesn't work we're in big trouble since the input stream
          // won't be aligned correctly.  I'm not sure quite what to
          // do at this point.  It's probably best just to return -1.
          if (ACE::recv (handle, &read_p[n], remainder) <= 0)
            return -1;
        }

      return 1;
    }

  // Return -1 if things have gone seriously wrong.
  if (n <= 0 && (errno != EWOULDBLOCK && errno != EAGAIN))
    return -1;

  return have_one ? 1 : 0;
}


int
ACE_Dev_Poll_Reactor_Notify::handle_input (ACE_HANDLE handle)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::handle_input");

  // @@ We may end up dispatching this event handler twice:  once when
  //    performing the speculative read on the notification pipe
  //    handle, and once more when dispatching the IO events.

  // Precondition: this->select_reactor_.token_.current_owner () ==
  // ACE_Thread::self ();

  int number_dispatched = 0;
  int result = 0;
  ACE_Notification_Buffer buffer;

  while ((result = this->read_notify_pipe (handle, buffer)) > 0)
    {
      // Dispatch the buffer
      // NOTE: We count only if we made any dispatches ie. upcalls.
      if (this->dispatch_notify (buffer) > 0)
        ++number_dispatched;

      // Bail out if we've reached the <notify_threshold_>.  Note that
      // by default <notify_threshold_> is -1, so we'll loop until all
      // the available notifications have been dispatched.
      if (number_dispatched == this->max_notify_iterations_)
        break;
    }

  if (result == -1)
    {
      // Reassign number_dispatched to -1 if things have gone
      // seriously wrong.
      number_dispatched = -1;
    }

  // Enqueue ourselves into the list of waiting threads.  When we
  // reacquire the token we'll be off and running again with ownership
  // of the token.  The postcondition of this call is that
  // <select_reactor_.token_.current_owner> == <ACE_Thread::self>.
  //this->select_reactor_->renew ();

  return number_dispatched;
}

ACE_HANDLE
ACE_Dev_Poll_Reactor_Notify::notify_handle (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::notify_handle");

  return this->notification_pipe_.read_handle ();
}

int
ACE_Dev_Poll_Reactor_Notify::is_dispatchable (ACE_Notification_Buffer &)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::is_dispatchable");

  ACE_NOTSUP_RETURN (-1);
}

int
ACE_Dev_Poll_Reactor_Notify::dispatch_notify (ACE_Notification_Buffer &buffer)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::dispatch_notify");

  // If eh == 0 then another thread is unblocking the
  // ACE_Dev_Poll_Reactor to update the ACE_Dev_Poll_Reactor's
  // internal structures.  Otherwise, we need to dispatch the
  // appropriate handle_* method on the ACE_Event_Handler
  // pointer we've been passed.
  if (buffer.eh_ != 0)
    {
      int result = 0;

      // Guard the handler's refcount. Recall that when the notify
      // was queued, the refcount was incremented, so it need not be
      // now. The guard insures that it is decremented properly.
      ACE_Dev_Poll_Handler_Guard eh_guard (buffer.eh_, false);

      switch (buffer.mask_)
        {
        case ACE_Event_Handler::READ_MASK:
        case ACE_Event_Handler::ACCEPT_MASK:
          result = buffer.eh_->handle_input (ACE_INVALID_HANDLE);
          break;
        case ACE_Event_Handler::WRITE_MASK:
          result = buffer.eh_->handle_output (ACE_INVALID_HANDLE);
          break;
        case ACE_Event_Handler::EXCEPT_MASK:
          result = buffer.eh_->handle_exception (ACE_INVALID_HANDLE);
          break;
        default:
          // Should we bail out if we get an invalid mask?
          ACE_ERROR ((LM_ERROR,
                      ACE_TEXT ("dispatch_notify invalid mask = %d\n"),
                      buffer.mask_));
        }
      if (result == -1)
        buffer.eh_->handle_close (ACE_INVALID_HANDLE, buffer.mask_);
    }

  return 1;
}

void
ACE_Dev_Poll_Reactor_Notify::max_notify_iterations (int iterations)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::max_notify_iterations");

  // Must always be > 0 or < 0 to optimize the loop exit condition.
  if (iterations == 0)
    iterations = 1;

  this->max_notify_iterations_ = iterations;
}

int
ACE_Dev_Poll_Reactor_Notify::max_notify_iterations (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::max_notify_iterations");

  return this->max_notify_iterations_;
}

int
ACE_Dev_Poll_Reactor_Notify::purge_pending_notifications (
  ACE_Event_Handler *eh,
  ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::purge_pending_notifications");

#if defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE)

  return notification_queue_.purge_pending_notifications (eh, mask);

#else /* defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE) */
  ACE_UNUSED_ARG (eh);
  ACE_UNUSED_ARG (mask);
  ACE_NOTSUP_RETURN (-1);
#endif  /* defined (ACE_HAS_REACTOR_NOTIFICATION_QUEUE) */
}

void
ACE_Dev_Poll_Reactor_Notify::dump (void) const
{
#if defined (ACE_HAS_DUMP)
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Notify::dump");

  ACE_DEBUG ((LM_DEBUG, ACE_BEGIN_DUMP, this));
  ACE_DEBUG ((LM_DEBUG,
              ACE_TEXT ("dp_reactor_ = %@"),
              this->dp_reactor_));
  this->notification_pipe_.dump ();
  ACE_DEBUG ((LM_DEBUG, ACE_END_DUMP));
#endif /* ACE_HAS_DUMP */
}

// -----------------------------------------------------------------

ACE_Dev_Poll_Reactor_Handler_Repository::
ACE_Dev_Poll_Reactor_Handler_Repository (void)
  : max_size_ (0),
    handlers_ (0)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::ACE_Dev_Poll_Reactor_Handler_Repository");
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::invalid_handle (
  ACE_HANDLE handle) const
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::invalid_handle");

  if (handle < 0 || handle >= this->max_size_)
    {
      errno = EINVAL;
      return 1;
    }
  else
    return 0;
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::handle_in_range (
  ACE_HANDLE handle) const
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::handle_in_range");

  if (handle >= 0 && handle < this->max_size_)
    return 1;
  else
    {
      errno = EINVAL;
      return 0;
    }
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::open (size_t size)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::open");

  this->max_size_ = size;

  // Try to allocate the memory.
  ACE_NEW_RETURN (this->handlers_,
                  ACE_Dev_Poll_Event_Tuple[size],
                  -1);

  // Try to increase the number of handles if <size> is greater than
  // the current limit.
  return ACE::set_handle_limit (size);
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::unbind_all (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::unbind_all");

  // Unbind all of the event handlers.
  for (int handle = 0;
       handle < this->max_size_;
       ++handle)
    this->unbind (handle);

  return 0;
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::close (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::close");

  if (this->handlers_ != 0)
    {
      this->unbind_all ();

      delete [] this->handlers_;
      this->handlers_ = 0;
    }

  return 0;
}

ACE_Event_Handler *
ACE_Dev_Poll_Reactor_Handler_Repository::find (ACE_HANDLE handle,
                                               size_t *index_p)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::find");

  ACE_Event_Handler *eh = 0;

  // Only bother to search for the <handle> if it's in range.
  if (this->handle_in_range (handle))
    {
      eh = this->handlers_[handle].event_handler;
      if (eh != 0)
        {
          if (index_p != 0)
            *index_p = handle;
        }
      else
        errno = ENOENT;
    }

  return eh;
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::bind (
  ACE_HANDLE handle,
  ACE_Event_Handler *event_handler,
  ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::bind");

  if (event_handler == 0)
    return -1;

  if (handle == ACE_INVALID_HANDLE)
    handle = event_handler->get_handle ();

  if (this->invalid_handle (handle))
    return -1;

  this->handlers_[handle].event_handler = event_handler;
  this->handlers_[handle].mask = mask;
  event_handler->add_reference ();

  return 0;
}

int
ACE_Dev_Poll_Reactor_Handler_Repository::unbind (ACE_HANDLE handle,
                                                 bool decr_refcnt)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor_Handler_Repository::unbind");

  if (this->find (handle) == 0)
    return -1;

  if (decr_refcnt)
    this->handlers_[handle].event_handler->remove_reference ();
  this->handlers_[handle].event_handler = 0;
  this->handlers_[handle].mask = ACE_Event_Handler::NULL_MASK;
  this->handlers_[handle].suspended = 0;

  return 0;
}

// -----------------------------------------------------------------

ACE_Dev_Poll_Reactor::ACE_Dev_Poll_Reactor (ACE_Sig_Handler *sh,
                                            ACE_Timer_Queue *tq,
                                            int disable_notify_pipe,
                                            ACE_Reactor_Notify *notify,
                                            int mask_signals,
                                            int s_queue)
  : initialized_ (false)
  , poll_fd_ (ACE_INVALID_HANDLE)
  , size_ (0)
  // , ready_set_ ()
#if defined (ACE_HAS_EVENT_POLL)
  , events_ (0)
  , start_pevents_ (0)
  , end_pevents_ (0)
#else
  , dp_fds_ (0)
  , start_pfds_ (0)
  , end_pfds_ (0)
#endif  /* ACE_HAS_EVENT_POLL */
  , deactivated_ (0)
  , token_ (*this, s_queue)
  , lock_adapter_ (token_)
  , timer_queue_ (0)
  , delete_timer_queue_ (false)
  , signal_handler_ (0)
  , delete_signal_handler_ (false)
  , notify_handler_ (0)
  , delete_notify_handler_ (false)
  , mask_signals_ (mask_signals)
  , restart_ (0)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::ACE_Dev_Poll_Reactor");

  if (this->open (ACE::max_handles (),
                  0,
                  sh,
                  tq,
                  disable_notify_pipe,
                  notify) == -1)
    ACE_ERROR ((LM_ERROR,
                ACE_TEXT ("%p\n"),
                ACE_TEXT ("ACE_Dev_Poll_Reactor::open ")
                ACE_TEXT ("failed inside ")
                ACE_TEXT ("ACE_Dev_Poll_Reactor::CTOR")));
}

ACE_Dev_Poll_Reactor::ACE_Dev_Poll_Reactor (size_t size,
                                            int rs,
                                            ACE_Sig_Handler *sh,
                                            ACE_Timer_Queue *tq,
                                            int disable_notify_pipe,
                                            ACE_Reactor_Notify *notify,
                                            int mask_signals,
                                            int s_queue)
  : initialized_ (false)
  , poll_fd_ (ACE_INVALID_HANDLE)
  , size_ (0)
  // , ready_set_ ()
#if defined (ACE_HAS_EVENT_POLL)
  , events_ (0)
  , start_pevents_ (0)
  , end_pevents_ (0)
#else
  , dp_fds_ (0)
  , start_pfds_ (0)
  , end_pfds_ (0)
#endif  /* ACE_HAS_EVENT_POLL */
  , deactivated_ (0)
  , token_ (*this, s_queue)
  , lock_adapter_ (token_)
  , timer_queue_ (0)
  , delete_timer_queue_ (false)
  , signal_handler_ (0)
  , delete_signal_handler_ (false)
  , notify_handler_ (0)
  , delete_notify_handler_ (false)
  , mask_signals_ (mask_signals)
  , restart_ (0)
{
  if (this->open (size,
                  rs,
                  sh,
                  tq,
                  disable_notify_pipe,
                  notify) == -1)
    ACE_ERROR ((LM_ERROR,
                ACE_TEXT ("%p\n"),
                ACE_TEXT ("ACE_Dev_Poll_Reactor::open ")
                ACE_TEXT ("failed inside ACE_Dev_Poll_Reactor::CTOR")));
}

ACE_Dev_Poll_Reactor::~ACE_Dev_Poll_Reactor (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::~ACE_Dev_Poll_Reactor");

 (void) this->close ();
}

int
ACE_Dev_Poll_Reactor::open (size_t size,
                            bool restart,
                            ACE_Sig_Handler *sh,
                            ACE_Timer_Queue *tq,
                            int disable_notify_pipe,
                            ACE_Reactor_Notify *notify)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::open");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  // Can't initialize ourselves more than once.
  if (this->initialized_)
    return -1;

  this->restart_ = restart;
  this->signal_handler_ = sh;
  this->timer_queue_ = tq;
  this->notify_handler_ = notify;

  int result = 0;

  // Allows the signal handler to be overridden.
  if (this->signal_handler_ == 0)
    {
      ACE_NEW_RETURN (this->signal_handler_,
                      ACE_Sig_Handler,
                      -1);

      if (this->signal_handler_ == 0)
        result = -1;
      else
        this->delete_signal_handler_ = true;
    }

  // Allows the timer queue to be overridden.
  if (result != -1 && this->timer_queue_ == 0)
    {
      ACE_NEW_RETURN (this->timer_queue_,
                      ACE_Timer_Heap,
                      -1);

      if (this->timer_queue_ == 0)
        result = -1;
      else
        this->delete_timer_queue_ = true;
    }

  // Allows the Notify_Handler to be overridden.
  if (result != -1 && this->notify_handler_ == 0)
    {
      ACE_NEW_RETURN (this->notify_handler_,
                      ACE_Dev_Poll_Reactor_Notify,
                      -1);

      if (this->notify_handler_ == 0)
        result = -1;
      else
        this->delete_notify_handler_ = true;
    }

#if defined (ACE_HAS_EVENT_POLL)

  // Allocating event table:
  ACE_NEW_RETURN (this->events_, epoll_event[size], -1);

  // Initializing epoll:
  this->poll_fd_ = ::epoll_create (size);
  if (this->poll_fd_ == -1)
    result = -1;

#else

  // Allocate the array before opening the device to avoid a potential
  // resource leak if allocation fails.
  ACE_NEW_RETURN (this->dp_fds_,
                  pollfd[size],
                  -1);

  // Open the `/dev/poll' character device.
  this->poll_fd_ = ACE_OS::open ("/dev/poll", O_RDWR);
  if (this->poll_fd_ == ACE_INVALID_HANDLE)
    result = -1;

#endif  /* ACE_HAS_EVENT_POLL */

  if (result != -1 && this->handler_rep_.open (size) == -1)
    result = -1;

  // Registration of the notification handler must be done after the
  // /dev/poll device has been fully initialized.
  else if (this->notify_handler_->open (this,
                                        0,
                                        disable_notify_pipe) == -1
           || (disable_notify_pipe == 0
               && this->register_handler_i (
                                            this->notify_handler_->notify_handle (),
                                            this->notify_handler_,
                                            ACE_Event_Handler::READ_MASK) == -1))
    result = -1;

  this->size_ = size;

  if (result != -1)
    // We're all set to go.
    this->initialized_ = true;
  else
    // This will close down all the allocated resources properly.
 (void) this->close ();

  return result;
}

int
ACE_Dev_Poll_Reactor::current_info (ACE_HANDLE, size_t & /* size */)
{
  ACE_NOTSUP_RETURN (-1);
}


int
ACE_Dev_Poll_Reactor::set_sig_handler (ACE_Sig_Handler *signal_handler)
{
  if (this->delete_signal_handler_)
    delete this->signal_handler_;

  this->signal_handler_ = signal_handler;
  this->delete_signal_handler_ = false;

  return 0;
}

int
ACE_Dev_Poll_Reactor::timer_queue (ACE_Timer_Queue *tq)
{
  if (this->delete_timer_queue_)
    delete this->timer_queue_;

  this->timer_queue_ = tq;
  this->delete_timer_queue_ = false;

  return 0;

}

ACE_Timer_Queue *
ACE_Dev_Poll_Reactor::timer_queue (void) const
{
  return this->timer_queue_;
}

int
ACE_Dev_Poll_Reactor::close (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::close");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  int result = 0;

  if (this->poll_fd_ != ACE_INVALID_HANDLE)
    {
      result = ACE_OS::close (this->poll_fd_);
    }

#if defined (ACE_HAS_EVENT_POLL)

  delete [] this->events_;
  this->events_ = 0;

#else

  delete [] this->dp_fds_;
  this->dp_fds_ = 0;

#endif  /* ACE_HAS_EVENT_POLL */

  if (this->delete_signal_handler_)
    {
      delete this->signal_handler_;
      this->signal_handler_ = 0;
      this->delete_signal_handler_ = false;
    }

 (void) this->handler_rep_.close ();

  if (this->delete_timer_queue_)
    {
      delete this->timer_queue_;
      this->timer_queue_ = 0;
      this->delete_timer_queue_ = false;
    }

  if (this->notify_handler_ != 0)
    this->notify_handler_->close ();

  if (this->delete_notify_handler_)
    {
      delete this->notify_handler_;
      this->notify_handler_ = 0;
      this->delete_notify_handler_ = false;
    }

  this->poll_fd_ = ACE_INVALID_HANDLE;

#if defined (ACE_HAS_EVENT_POLL)
  this->start_pevents_ = 0;
  this->end_pevents_   = 0;
#else
  this->start_pfds_ = 0;
  this->end_pfds_ = 0;
#endif /* ACE_HAS_EVENT_POLL */

  this->initialized_ = false;

  return result;
}

int
ACE_Dev_Poll_Reactor::work_pending (const ACE_Time_Value & max_wait_time)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::work_pending");

  // Stash the current time
  //
  // The destructor of this object will automatically compute how much
  // time elapsed since this method was called.
  ACE_Time_Value mwt (max_wait_time);
  ACE_MT (ACE_Countdown_Time countdown (&mwt));

  Token_Guard guard (this->token_);
  int const result = guard.acquire_quietly (&mwt);

  // If the guard is NOT the owner just return the retval
  if (!guard.is_owner ())
    return result;

  // Update the countdown to reflect time waiting for the mutex.
  ACE_MT (countdown.update ());

  return this->work_pending_i (&mwt);
}

int
ACE_Dev_Poll_Reactor::work_pending_i (ACE_Time_Value * max_wait_time)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::work_pending_i");

  if (this->deactivated_)
    return 0;

#if defined (ACE_HAS_EVENT_POLL)
  if (this->start_pevents_ != this->end_pevents_)
#else
  if (this->start_pfds_ != this->end_pfds_)
#endif /* ACE_HAS_EVENT_POLL */
    return 1;  // We still have work_pending (). Do not poll for
               // additional events.

  ACE_Time_Value timer_buf (0);
  ACE_Time_Value *this_timeout =
    this->timer_queue_->calculate_timeout (max_wait_time, &timer_buf);

  // Check if we have timers to fire.
  int const timers_pending =
 ((this_timeout != 0 && max_wait_time == 0)
     || (this_timeout != 0 && max_wait_time != 0
         && *this_timeout != *max_wait_time) ? 1 : 0);

  long const timeout =
 (this_timeout == 0
     ? -1 /* Infinity */
     : static_cast<long> (this_timeout->msec ()));

#if defined (ACE_HAS_EVENT_POLL)

   // Wait for events.
   int const nfds = ::epoll_wait (this->poll_fd_,
                                  this->events_,
                                  this->size_,
                                  static_cast<int> (timeout));

  if (nfds > 0)
    {
      this->start_pevents_ = this->events_;
      this->end_pevents_ = this->start_pevents_ + nfds;
    }

#else

  struct dvpoll dvp;

  dvp.dp_fds = this->dp_fds_;
  dvp.dp_nfds = this->size_;
  dvp.dp_timeout = timeout;  // Milliseconds

  // Poll for events
  int const nfds = ACE_OS::ioctl (this->poll_fd_, DP_POLL, &dvp);

  // Retrieve the results from the pollfd array.
  this->start_pfds_ = dvp.dp_fds;

  // If nfds == 0 then end_pfds_ == start_pfds_ meaning that there is
  // no work pending.  If nfds > 0 then there is work pending.
  // Otherwise an error occurred.
  if (nfds > -1)
    this->end_pfds_ = this->start_pfds_ + nfds;
#endif  /* ACE_HAS_EVENT_POLL */

  // If timers are pending, override any timeout from the poll.
  return (nfds == 0 && timers_pending != 0 ? 1 : nfds);
}


int
ACE_Dev_Poll_Reactor::handle_events (ACE_Time_Value *max_wait_time)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::handle_events");

  // Stash the current time
  //
  // The destructor of this object will automatically compute how much
  // time elapsed since this method was called.
  ACE_MT (ACE_Countdown_Time countdown (max_wait_time));

  Token_Guard guard (this->token_);
  int const result = guard.acquire_quietly (max_wait_time);

  // If the guard is NOT the owner just return the retval
  if (!guard.is_owner ())
    return result;

  if (this->deactivated_)
    return -1;

  // Update the countdown to reflect time waiting for the mutex.
  ACE_MT (countdown.update ());

  return this->handle_events_i (max_wait_time, guard);
}

int
ACE_Dev_Poll_Reactor::handle_events_i (ACE_Time_Value *max_wait_time,
                                       Token_Guard &guard)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::handle_events_i");

  int result = 0;
  // int active_handle_count = 0;

  // Poll for events
  //
  // If the underlying ioctl () call was interrupted via the interrupt
  // signal (i.e. returned -1 with errno == EINTR) then the loop will
  // be restarted if so desired.
  do
    {
      result = this->work_pending_i (max_wait_time);
      if (result == -1)
        ACE_ERROR ((LM_ERROR, "%t: %p\n", "work_pending_i"));
    }
  while (result == -1 && this->restart_ != 0 && errno == EINTR);

  if (result == 0 || (result == -1 && errno == ETIME))
    return 0;
  else if (result == -1)
    {
      if (errno != EINTR)
        return -1;

      // Bail out -- we got here since the poll was interrupted.
      // If it was due to a signal registered through our ACE_Sig_Handler,
      // then it was dispatched, so we count it in the number of events
      // handled rather than cause an error return.
      if (ACE_Sig_Handler::sig_pending () != 0)
        {
          ACE_Sig_Handler::sig_pending (0);
          return 1;
        }
      return -1;
    }

  // Dispatch an event.
  return this->dispatch (guard);
}

// Dispatch an event. On entry, the token is held by the caller. If an
// event is found to dispatch, the token is released before dispatching it.
int
ACE_Dev_Poll_Reactor::dispatch (Token_Guard &guard)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::dispatch");

  // Perform the Template Method for dispatching the first located event.
  // We dispatch only one to effectively dispatch events concurrently.
  // As soon as an event is located, the token is released, allowing the
  // next waiter to begin getting an event while we dispatch one here.
  int result = 0;

  // Handle timers early since they may have higher latency
  // constraints than I/O handlers.  Ideally, the order of
  // dispatching should be a strategy...
  if ((result = this->dispatch_timer_handler (guard)) != 0)
    return result;

  // Check to see if there are no more I/O handles left to
  // dispatch AFTER we've handled the timers.

  // Finally, dispatch the I/O handlers.
  result = this->dispatch_io_event (guard);

  return result;
}

int
ACE_Dev_Poll_Reactor::dispatch_timer_handler (Token_Guard &guard)
{
  if (this->timer_queue_->is_empty ())
    return 0;       // Empty timer queue so cannot have any expired timers.

  // Get the current time
  ACE_Time_Value cur_time (this->timer_queue_->gettimeofday () +
                           this->timer_queue_->timer_skew ());

  // Look for a node in the timer queue whose timer <= the present
  // time.
  ACE_Timer_Node_Dispatch_Info info;
  if (this->timer_queue_->dispatch_info (cur_time, info))
    {
      const void *upcall_act = 0;

      // Preinvoke (handles refcount if needed, etc.)
      this->timer_queue_->preinvoke (info, cur_time, upcall_act);

      // Release the token before expiration upcall.
      guard.release_token ();

      // call the functor
      this->timer_queue_->upcall (info, cur_time);

      // Postinvoke (undo refcount if needed, etc.)
      this->timer_queue_->postinvoke (info, cur_time, upcall_act);

      // We have dispatched a timer
      return 1;
    }

  return 0;
}

#if 0
int
ACE_Dev_Poll_Reactor::dispatch_notification_handlers (
  ACE_Select_Reactor_Handle_Set &dispatch_set,
  int &number_of_active_handles,
  int &number_of_handlers_dispatched)
{
  // Check to see if the ACE_HANDLE associated with the
  // Dev_Poll_Reactor's notify hook is enabled.  If so, it means that
  // one or more other threads are trying to update the
  // ACE_Dev_Poll_Reactor's internal tables or the notify pipe is
  // enabled.  We'll handle all these threads and notifications, and
  // then break out to continue the event loop.

  const int n =
    this->notify_handler_->dispatch_notifications (number_of_active_handles,
                                                   dispatch_set.rd_mask_);

  if (n == -1)
    return -1;
  else
    number_of_handlers_dispatched += n;

  return /* this->state_changed_ ? -1 : */ 0;
}
#endif  /* 0 */

int
ACE_Dev_Poll_Reactor::dispatch_io_event (Token_Guard &guard)
{

  // Define bits to check for while dispatching.
#if defined (ACE_HAS_EVENT_POLL)
  const __uint32_t out_event = EPOLLOUT;
  const __uint32_t exc_event = EPOLLPRI;
  const __uint32_t in_event  = EPOLLIN;
  const __uint32_t err_event = EPOLLHUP | EPOLLERR;
#else
  const short out_event = POLLOUT;
  const short exc_event = POLLPRI;
  const short in_event  = POLLIN;
  const short err_event = 0;              // No known bits for this
#endif /* ACE_HAS_EVENT_POLL */

  // Since the underlying event demultiplexing mechansim (`/dev/poll'
  // or '/dev/epoll') is stateful, and since only one result buffer is
  // used, all pending events (i.e. those retrieved from a previous
  // poll) must be dispatched before any additional event can be
  // polled.  As such, the Dev_Poll_Reactor keeps track of the
  // progress of events that have been dispatched.

  // Dispatch the events.
  //
  // Select the first available handle with event (s) pending. Check for
  // event type in defined order of dispatch: output, exception, input.
  // When an event is located, clear its bit in the dispatch set. If there
  // are no more events for the handle, also increment the pfds pointer
  // to move to the next handle ready.
  //
  // Notice that pfds only contains file descriptors that have
  // received events.
#if defined (ACE_HAS_EVENT_POLL)
  struct epoll_event *& pfds = this->start_pevents_;
  if (pfds < this->end_pevents_)
#else
  struct pollfd *& pfds = this->start_pfds_;
  if (pfds < this->end_pfds_)
#endif /* ACE_HAS_EVENT_POLL */
    {
#if defined (ACE_HAS_EVENT_POLL)
      const ACE_HANDLE handle   = pfds->data.fd;
      __uint32_t &revents       = pfds->events;
#else
      const ACE_HANDLE handle = pfds->fd;
      short &revents          = pfds->revents;
#endif /* ACE_HAS_EVENT_POLL */

      // Figure out what to do first in order to make it easier to manage
      // the bit twiddling and possible pfds increment before releasing
      // the token for dispatch.
      // Note that if there's an error (such as the handle was closed
      // without being removed from the event set) the EPOLLHUP and/or
      // EPOLLERR bits will be set in revents.
      bool disp_out = false;
      bool disp_exc = false;
      bool disp_in  = false;
      if (ACE_BIT_ENABLED (revents, out_event))
        {
          disp_out = true;
          ACE_CLR_BITS (revents, out_event);
        }
      else if (ACE_BIT_ENABLED (revents, exc_event))
        {
          disp_exc = true;
          ACE_CLR_BITS (revents, exc_event);
        }
      else if (ACE_BIT_ENABLED (revents, in_event))
        {
          disp_in = true;
          ACE_CLR_BITS (revents, in_event);
        }
      else if (ACE_BIT_ENABLED (revents, err_event))
        {
          this->remove_handler_i (handle, ACE_Event_Handler::ALL_EVENTS_MASK);
          ++pfds;
          return 1;
        }
      else
        {
          ACE_ERROR ((LM_ERROR, ACE_TEXT (" (%t) dispatch_io h %d unknown events 0x%x\n"), handle, revents));
          // ACE_ASSERT (0);
        }

      // Increment the pointer to the next element before we
      // release the token.  Otherwise event handlers end up being
      // dispatched multiple times for the same poll.
      if (revents == 0)
        ++pfds;

      /* When using sys_epoll, we can attach arbitrary user
         data to the descriptor, so it can be delivered when
         activity is detected. Perhaps we should store event
         handler together with descriptor, instead of looking
         it up in a repository ? Could it boost performance ?
      */
      ACE_Event_Handler *eh = this->handler_rep_.find (handle);

      if (eh)
        {
          // Modify the reference count in an exception-safe way.
          // Note that eh could be the notify handler. It's not strictly
          // necessary to manage its refcount, but since we don't enable
          // the counting policy, it won't do much. Management of the
          // notified handlers themselves is done in the notify handler.
          ACE_Dev_Poll_Handler_Guard eh_guard (eh);

          // Release the reactor token before upcall.
          guard.release_token ();

          // Dispatch the detected event
          if (disp_out)
            {
              const int status =
                this->upcall (eh, &ACE_Event_Handler::handle_output, handle);

              if (status < 0)
                // Note that the token is reacquired in remove_handler ().
                this->remove_handler (handle, ACE_Event_Handler::WRITE_MASK);
              return 1;
            }

          if (disp_exc)
            {
              const int status =
                this->upcall (eh, &ACE_Event_Handler::handle_exception, handle);

              if (status < 0)
                // Note that the token is reacquired in remove_handler ().
                this->remove_handler (handle, ACE_Event_Handler::EXCEPT_MASK);
              return 1;
            }

          if (disp_in)
            {
              const int status =
                this->upcall (eh, &ACE_Event_Handler::handle_input, handle);

              if (status < 0)
                // Note that the token is reacquired in remove_handler ().
                this->remove_handler (handle, ACE_Event_Handler::READ_MASK);
              return 1;
            }
        } // The reactor token is reacquired upon leaving this scope.
    }

  return 0;
}

int
ACE_Dev_Poll_Reactor::alertable_handle_events (ACE_Time_Value *max_wait_time)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::alertable_handle_events");

  return this->handle_events (max_wait_time);
}

int
ACE_Dev_Poll_Reactor::handle_events (ACE_Time_Value &max_wait_time)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::handle_events");

  return this->handle_events (&max_wait_time);
}

int
ACE_Dev_Poll_Reactor::alertable_handle_events (ACE_Time_Value &max_wait_time)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::alertable_handle_events");

  return this->handle_events (max_wait_time);
}

int
ACE_Dev_Poll_Reactor::deactivated (void)
{
  return this->deactivated_;
}

void
ACE_Dev_Poll_Reactor::deactivate (int do_stop)
{
  this->deactivated_ = do_stop;
  this->wakeup_all_threads ();
}

int
ACE_Dev_Poll_Reactor::register_handler (ACE_Event_Handler *handler,
                                        ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::register_handler");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->register_handler_i (handler->get_handle (),
                                   handler,
                                   mask);
}

int
ACE_Dev_Poll_Reactor::register_handler (ACE_HANDLE handle,
                                        ACE_Event_Handler *event_handler,
                                        ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::register_handler");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->register_handler_i (handle,
                                   event_handler,
                                   mask);
}

int
ACE_Dev_Poll_Reactor::register_handler_i (ACE_HANDLE handle,
                                          ACE_Event_Handler *event_handler,
                                          ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::register_handler_i");

  if (handle == ACE_INVALID_HANDLE
      || mask == ACE_Event_Handler::NULL_MASK)
    {
      errno = EINVAL;
      return -1;
    }

 if (this->handler_rep_.find (handle) == 0)
   {
     // Handler not present in the repository.  Bind it.
     if (this->handler_rep_.bind (handle, event_handler, mask) != 0)
       return -1;

#if defined (ACE_HAS_EVENT_POLL)

     struct epoll_event epev;
     ACE_OS::memset (&epev, 0, sizeof (epev));
     static const int op = EPOLL_CTL_ADD;

     epev.events  = this->reactor_mask_to_poll_event (mask);
     epev.data.fd = handle;

     if (::epoll_ctl (this->poll_fd_, op, handle, &epev) == -1)
       {
         ACE_ERROR ((LM_ERROR, "%p\n", "epoll_ctl"));
 (void) this->handler_rep_.unbind (handle);
         return -1;
       }

#endif /* ACE_HAS_EVENT_POLL */
   }
 else
   {
     // Handler is already present in the repository, so register it
     // again, possibly for different event.  Add new mask to the
     // current one.
     if (this->mask_ops_i (handle, mask, ACE_Reactor::ADD_MASK) == -1)
       ACE_ERROR_RETURN ((LM_ERROR, "%p\n", "mask_ops_i"), -1);
   }

#ifndef  ACE_HAS_EVENT_POLL

  struct pollfd pfd;

  pfd.fd      = handle;
  pfd.events  = this->reactor_mask_to_poll_event (mask);
  pfd.revents = 0;

  // Add file descriptor to the "interest set."
  if (ACE_OS::write (this->poll_fd_, &pfd, sizeof (pfd)) != sizeof (pfd))
    {
 (void) this->handler_rep_.unbind (handle);
      return -1;
    }
#endif /*ACE_HAS_EVENT_POLL*/

  // Note the fact that we've changed the state of the wait_set_,
  // which is used by the dispatching loop to determine whether it can
  // keep going or if it needs to reconsult select ().
  // this->state_changed_ = 1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::register_handler (
  ACE_HANDLE /* event_handle */,
  ACE_HANDLE /* io_handle */,
  ACE_Event_Handler * /* event_handler */,
  ACE_Reactor_Mask /* mask */)
{
  ACE_NOTSUP_RETURN (-1);
}

int
ACE_Dev_Poll_Reactor::register_handler (const ACE_Handle_Set &handle_set,
                                        ACE_Event_Handler *event_handler,
                                        ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::register_handler");

  ACE_Handle_Set_Iterator handle_iter (handle_set);

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  // @@ It might be more efficient to construct a pollfd array and
  //    pass it to the write () call in register_handler_i () only once,
  //    instead of calling write () (a system call) once for each file
  //    descriptor.

  for (ACE_HANDLE h = handle_iter ();
       h != ACE_INVALID_HANDLE;
       h = handle_iter ())
    if (this->register_handler_i (h, event_handler, mask) == -1)
      return -1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::register_handler (int signum,
                                        ACE_Event_Handler *new_sh,
                                        ACE_Sig_Action *new_disp,
                                        ACE_Event_Handler **old_sh,
                                        ACE_Sig_Action *old_disp)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::register_handler");

  return this->signal_handler_->register_handler (signum,
                                                  new_sh,
                                                  new_disp,
                                                  old_sh,
                                                  old_disp);
}

int
ACE_Dev_Poll_Reactor::register_handler (const ACE_Sig_Set &sigset,
                                        ACE_Event_Handler *new_sh,
                                        ACE_Sig_Action *new_disp)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::register_handler");

  int result = 0;

#if (ACE_NSIG > 0)

  for (int s = 1; s < ACE_NSIG; ++s)
    if ((sigset.is_member (s) == 1)
        && this->signal_handler_->register_handler (s,
                                                    new_sh,
                                                    new_disp) == -1)
      result = -1;

#else  /* ACE_NSIG <= 0 */

  ACE_UNUSED_ARG (sigset);
  ACE_UNUSED_ARG (new_sh);
  ACE_UNUSED_ARG (new_disp);

#endif /* ACE_NSIG <= 0  */

  return result;
}

int
ACE_Dev_Poll_Reactor::remove_handler (ACE_Event_Handler *handler,
                                      ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::remove_handler");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->remove_handler_i (handler->get_handle (), mask);
}

int
ACE_Dev_Poll_Reactor::remove_handler (ACE_HANDLE handle,
                                      ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::remove_handler");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->remove_handler_i (handle, mask);
}

int
ACE_Dev_Poll_Reactor::remove_handler_i (ACE_HANDLE handle,
                                        ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::remove_handler_i");

  ACE_Event_Handler *eh = this->handler_rep_.find (handle);

  if (eh == 0 ||
      this->mask_ops_i (handle, mask, ACE_Reactor::CLR_MASK) == -1)
    return -1;

  // Check for ref counting now - handle_close () may delete eh.
  bool const requires_reference_counting =
    eh->reference_counting_policy ().value () ==
    ACE_Event_Handler::Reference_Counting_Policy::ENABLED;

  if (ACE_BIT_DISABLED (mask, ACE_Event_Handler::DONT_CALL))
 (void) eh->handle_close (handle, mask);

  // If there are no longer any outstanding events on the given handle
  // then remove it from the handler repository.
  if (this->handler_rep_.mask (handle) == ACE_Event_Handler::NULL_MASK)
    this->handler_rep_.unbind (handle, requires_reference_counting);

  // Note the fact that we've changed the state of the wait_set,
  // i.e. the "interest set," which is used by the dispatching loop to
  // determine whether it can keep going or if it needs to reconsult
  // /dev/poll or /dev/epoll.
  // this->state_changed_ = 1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::remove_handler (const ACE_Handle_Set &handle_set,
                                      ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::remove_handler");

  ACE_Handle_Set_Iterator handle_iter (handle_set);

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  // @@ It might be more efficient to construct a pollfd array and
  //    pass it to the write () call in register_handler_i () only once,
  //    instead of calling write () (a system call) once for each file
  //    descriptor.

  for (ACE_HANDLE h = handle_iter ();
       h != ACE_INVALID_HANDLE;
       h = handle_iter ())
    if (this->remove_handler_i (h, mask) == -1)
      return -1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::remove_handler (int signum,
                                      ACE_Sig_Action *new_disp,
                                      ACE_Sig_Action *old_disp,
                                      int sigkey)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::remove_handler");

  return this->signal_handler_->remove_handler (signum,
                                                new_disp,
                                                old_disp,
                                                sigkey);
}

int
ACE_Dev_Poll_Reactor::remove_handler (const ACE_Sig_Set &sigset)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::remove_handler");

  int result = 0;

#if (ACE_NSIG > 0)

  for (int s = 1; s < ACE_NSIG; ++s)
    if ((sigset.is_member (s) == 1)
        && this->signal_handler_->remove_handler (s) == -1)
      result = -1;

#else  /* ACE_NSIG <= 0 */

  ACE_UNUSED_ARG (sigset);

#endif /* ACE_NSIG <= 0 */

  return result;
}

int
ACE_Dev_Poll_Reactor::suspend_handler (ACE_Event_Handler *event_handler)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::suspend_handler");

  if (event_handler == 0)
    {
      errno = EINVAL;
      return -1;
    }

  ACE_HANDLE handle = event_handler->get_handle ();

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->suspend_handler_i (handle);
}

int
ACE_Dev_Poll_Reactor::suspend_handler (ACE_HANDLE handle)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::suspend_handler");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->suspend_handler_i (handle);
}

int
ACE_Dev_Poll_Reactor::suspend_handler (const ACE_Handle_Set &handles)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::suspend_handler");

  ACE_Handle_Set_Iterator handle_iter (handles);
  ACE_HANDLE h;

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  while ((h = handle_iter ()) != ACE_INVALID_HANDLE)
    if (this->suspend_handler_i (h) == -1)
      return -1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::suspend_handlers (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::suspend_handlers");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  size_t const len = this->handler_rep_.size ();

  for (size_t i = 0; i < len; ++i)
    if (this->handler_rep_.suspended (i) == 0
        && this->suspend_handler_i (i) != 0)
      return -1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::suspend_handler_i (ACE_HANDLE handle)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::suspend_handler_i");

  if (this->handler_rep_.find (handle) == 0)
    return -1;

  if (this->handler_rep_.suspended (handle))
    return 0;  // Already suspended.  @@ Should this be an error?

  // Remove the handle from the "interest set."
  //
  // Note that the associated event handler is still in the handler
  // repository, but no events will be polled on the given handle thus
  // no event will be dispatched to the event handler.

#if defined (ACE_HAS_EVENT_POLL)

  struct epoll_event epev;
  ACE_OS::memset (&epev, 0, sizeof (epev));
  static const int op = EPOLL_CTL_DEL;

  epev.events  = 0;
  epev.data.fd = handle;

  if (::epoll_ctl (this->poll_fd_, op, handle, &epev) == -1)
    return -1;

#else

  struct pollfd pfd[1];

  pfd[0].fd      = handle;
  pfd[0].events  = POLLREMOVE;
  pfd[0].revents = 0;

  if (ACE_OS::write (this->poll_fd_, pfd, sizeof (pfd)) != sizeof (pfd))
    return -1;

#endif  /* ACE_HAS_EVENT_POLL */

  this->handler_rep_.suspend (handle);

  return 0;
}

int
ACE_Dev_Poll_Reactor::resume_handler (ACE_Event_Handler *event_handler)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::resume_handler");

  if (event_handler == 0)
    {
      errno = EINVAL;
      return -1;
    }

  ACE_HANDLE handle = event_handler->get_handle ();

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->resume_handler_i (handle);
}

int
ACE_Dev_Poll_Reactor::resume_handler (ACE_HANDLE handle)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::resume_handler");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->resume_handler_i (handle);
}

int
ACE_Dev_Poll_Reactor::resume_handler (const ACE_Handle_Set &handles)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::resume_handler");

  ACE_Handle_Set_Iterator handle_iter (handles);
  ACE_HANDLE h;

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  while ((h = handle_iter ()) != ACE_INVALID_HANDLE)
    if (this->resume_handler_i (h) == -1)
      return -1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::resume_handlers (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::resume_handlers");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  size_t const len = this->handler_rep_.size ();

  for (size_t i = 0; i < len; ++i)
    if (this->handler_rep_.suspended (i)
        && this->resume_handler_i (i) != 0)
      return -1;

  return 0;
}

int
ACE_Dev_Poll_Reactor::resume_handler_i (ACE_HANDLE handle)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::resume_handler_i");

  if (this->handler_rep_.find (handle) == 0
      && this->handler_rep_.suspended (handle) == 0)
    return -1;

  ACE_Reactor_Mask mask = this->handler_rep_.mask (handle);

  if (mask == ACE_Event_Handler::NULL_MASK)
    return -1;

  // Place the handle back in to the "interest set."
  //
  // Events for the given handle will once again be polled.

#if defined (ACE_HAS_EVENT_POLL)

  struct epoll_event epev;
  ACE_OS::memset (&epev, 0, sizeof (epev));
  static const int op = EPOLL_CTL_ADD;

  epev.events  = this->reactor_mask_to_poll_event (mask);
  epev.data.fd = handle;

  if (::epoll_ctl (this->poll_fd_, op, handle, &epev) == -1)
    return -1;

#else

  struct pollfd pfd[1];

  pfd[0].fd      = handle;
  pfd[0].events  = this->reactor_mask_to_poll_event (mask);
  pfd[0].revents = 0;

  if (ACE_OS::write (this->poll_fd_, pfd, sizeof (pfd)) != sizeof (pfd))
    return -1;

#endif  /* ACE_HAS_EVENT_POLL */

  this->handler_rep_.resume (handle);

  return 0;
}

int
ACE_Dev_Poll_Reactor::resumable_handler (void)
{
  // @@ Is this correct?

  return 0;
}

bool
ACE_Dev_Poll_Reactor::uses_event_associations (void)
{
  // Since the Dev_Poll_Reactor does not do any event associations,
  // this method always return false.
  return false;
}

long
ACE_Dev_Poll_Reactor::schedule_timer (ACE_Event_Handler *event_handler,
                                      const void *arg,
                                      const ACE_Time_Value &delay,
                                      const ACE_Time_Value &interval)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::schedule_timer");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  if (0 != this->timer_queue_)
    return this->timer_queue_->schedule
      (event_handler,
       arg,
       this->timer_queue_->gettimeofday () + delay,
       interval);

  errno = ESHUTDOWN;
  return -1;
}

int
ACE_Dev_Poll_Reactor::reset_timer_interval (long timer_id,
                                            const ACE_Time_Value &interval)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::reset_timer_interval");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  if (0 != this->timer_queue_)
    return this->timer_queue_->reset_interval (timer_id, interval);

  errno = ESHUTDOWN;
  return -1;
}

int
ACE_Dev_Poll_Reactor::cancel_timer (ACE_Event_Handler *event_handler,
                                    int dont_call_handle_close)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::cancel_timer");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return (this->timer_queue_ == 0
          ? 0
          : this->timer_queue_->cancel (event_handler,
                                        dont_call_handle_close));
}

int
ACE_Dev_Poll_Reactor::cancel_timer (long timer_id,
                                    const void **arg,
                                    int dont_call_handle_close)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::cancel_timer");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return (this->timer_queue_ == 0
          ? 0
          : this->timer_queue_->cancel (timer_id,
                                        arg,
                                        dont_call_handle_close));
}

int
ACE_Dev_Poll_Reactor::schedule_wakeup (ACE_Event_Handler *eh,
                                       ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::schedule_wakeup");

  return this->mask_ops (eh->get_handle (), mask, ACE_Reactor::ADD_MASK);
}

int
ACE_Dev_Poll_Reactor::schedule_wakeup (ACE_HANDLE handle,
                                       ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::schedule_wakeup");

  return this->mask_ops (handle, mask, ACE_Reactor::ADD_MASK);
}

int
ACE_Dev_Poll_Reactor::cancel_wakeup (ACE_Event_Handler *eh,
                                     ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::cancel_wakeup");

  return this->mask_ops (eh->get_handle (), mask, ACE_Reactor::CLR_MASK);
}

int
ACE_Dev_Poll_Reactor::cancel_wakeup (ACE_HANDLE handle,
                                     ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::cancel_wakeup");

  return this->mask_ops (handle, mask, ACE_Reactor::CLR_MASK);
}

int
ACE_Dev_Poll_Reactor::notify (ACE_Event_Handler *eh,
                              ACE_Reactor_Mask mask,
                              ACE_Time_Value *timeout)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::notify");

  ssize_t n = 0;

  // Pass over both the Event_Handler *and* the mask to allow the
  // caller to dictate which Event_Handler method the receiver
  // invokes.  Note that this call can timeout.

  n = this->notify_handler_->notify (eh, mask, timeout);

  return n == -1 ? -1 : 0;
}

void
ACE_Dev_Poll_Reactor::max_notify_iterations (int iterations)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::max_notify_iterations");

  ACE_MT (ACE_GUARD (ACE_Dev_Poll_Reactor_Token, mon, this->token_));

  this->notify_handler_->max_notify_iterations (iterations);
}

int
ACE_Dev_Poll_Reactor::max_notify_iterations (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::max_notify_iterations");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->notify_handler_->max_notify_iterations ();
}

int
ACE_Dev_Poll_Reactor::purge_pending_notifications (ACE_Event_Handler * eh,
                                                   ACE_Reactor_Mask mask)
{
  if (this->notify_handler_ == 0)
    return 0;

  return this->notify_handler_->purge_pending_notifications (eh, mask);
}

ACE_Event_Handler *
ACE_Dev_Poll_Reactor::find_handler (ACE_HANDLE handle)
{
  ACE_MT (ACE_READ_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, 0));

  ACE_Event_Handler *event_handler = this->handler_rep_.find (handle);
  if (event_handler)
    event_handler->add_reference ();
  return event_handler;
}

int
ACE_Dev_Poll_Reactor::handler (ACE_HANDLE handle,
                               ACE_Reactor_Mask mask,
                               ACE_Event_Handler **event_handler)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::handler");

  ACE_MT (ACE_READ_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  ACE_Event_Handler *h = this->handler_rep_.find (handle);

  if (h != 0
      && ACE_BIT_CMP_MASK (this->handler_rep_.mask (handle),
                           mask,  // Compare all bits in the mask
                           mask))
    {
      if (event_handler != 0)
        *event_handler = h;

      return 0;
    }

  return -1;
}

int
ACE_Dev_Poll_Reactor::handler (int signum,
                               ACE_Event_Handler **eh)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::handler");

  ACE_Event_Handler *handler = this->signal_handler_->handler (signum);

  if (handler == 0)
    return -1;
  else if (eh != 0)
    *eh = handler;

  return 0;
}

bool
ACE_Dev_Poll_Reactor::initialized (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::initialized");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, false));

  return this->initialized_;
}

size_t
ACE_Dev_Poll_Reactor::size (void) const
{
  return this->size_;
}

ACE_Lock &
ACE_Dev_Poll_Reactor::lock (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::lock");

  return this->lock_adapter_;
}

void
ACE_Dev_Poll_Reactor::wakeup_all_threads (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::wakeup_all_threads");

  // Send a notification, but don't block if there's no one to receive
  // it.
  this->notify (0,
                ACE_Event_Handler::NULL_MASK,
 (ACE_Time_Value *) &ACE_Time_Value::zero);
}

int
ACE_Dev_Poll_Reactor::owner (ACE_thread_t /* new_owner */,
                             ACE_thread_t * /* old_owner */)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::owner");

  // There is no need to set the owner of the event loop.  Multiple
  // threads may invoke the event loop simulataneously.

  return 0;
}

int
ACE_Dev_Poll_Reactor::owner (ACE_thread_t * /* owner */)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::owner");

  // There is no need to set the owner of the event loop.  Multiple
  // threads may invoke the event loop simulataneously.

  return 0;
}

bool
ACE_Dev_Poll_Reactor::restart (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::restart");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, false));

  return this->restart_;
}

bool
ACE_Dev_Poll_Reactor::restart (bool r)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::restart");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, false));

  bool current_value = this->restart_;
  this->restart_ = r;
  return current_value;
}

void
ACE_Dev_Poll_Reactor::requeue_position (int)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::requeue_position");
}

int
ACE_Dev_Poll_Reactor::requeue_position (void)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::requeue_position");

  ACE_NOTSUP_RETURN (-1);
}

int
ACE_Dev_Poll_Reactor::mask_ops (ACE_Event_Handler *event_handler,
                                ACE_Reactor_Mask mask,
                                int ops)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::mask_ops");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->mask_ops_i (event_handler->get_handle (), mask, ops);
}

int
ACE_Dev_Poll_Reactor::mask_ops (ACE_HANDLE handle,
                                ACE_Reactor_Mask mask,
                                int ops)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::mask_ops");

  ACE_MT (ACE_GUARD_RETURN (ACE_Dev_Poll_Reactor_Token, mon, this->token_, -1));

  return this->mask_ops_i (handle, mask, ops);
}

int
ACE_Dev_Poll_Reactor::mask_ops_i (ACE_HANDLE handle,
                                  ACE_Reactor_Mask mask,
                                  int ops)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::mask_ops_i");

  if (this->handler_rep_.handle_in_range (handle) == 0)
    return -1;

  // Block out all signals until method returns.
  ACE_Sig_Guard sb;

  ACE_Reactor_Mask const old_mask = this->handler_rep_.mask (handle);
  ACE_Reactor_Mask new_mask = old_mask;

  // Perform GET, CLR, SET, and ADD operations on the interest/wait
  // set and the suspend set (if necessary).
  //
  // GET = 1, Retrieve current value
  // SET = 2, Set value of bits to new mask (changes the entire mask)
  // ADD = 3, Bitwise "or" the value into the mask (only changes
  //          enabled bits)
  // CLR = 4  Bitwise "and" the negation of the value out of the mask
  // (only changes enabled bits)
  //
  // Returns the original mask.

  switch (ops)
    {
    case ACE_Reactor::GET_MASK:
      // The work for this operation is done in all cases at the
      // begining of the function.
      return old_mask;

    case ACE_Reactor::CLR_MASK:
      ACE_CLR_BITS (new_mask, mask);
      break;

    case ACE_Reactor::SET_MASK:
      new_mask = mask;
      break;

    case ACE_Reactor::ADD_MASK:
      ACE_SET_BITS (new_mask, mask);
      break;

    default:
      return -1;
    }

  /// Reset the mask for the given handle.
  this->handler_rep_.mask (handle, new_mask);

  if (this->handler_rep_.suspended (handle) == 0)
    {
      // Only attempt to alter events for the handle from the
      // "interest set" if it hasn't been suspended.

      short const events = this->reactor_mask_to_poll_event (new_mask);

#if defined (sun)
      // Apparently events cannot be updated on-the-fly on Solaris so
      // remove the existing events, and then add the new ones.
      struct pollfd pfd[2];

      pfd[0].fd      = handle;
      pfd[0].events  = POLLREMOVE;
      pfd[0].revents = 0;
      pfd[1].fd      = (events == POLLREMOVE ? ACE_INVALID_HANDLE : handle);
      pfd[1].events  = events;
      pfd[1].revents = 0;

      // Change the events associated with the given file descriptor.
      if (ACE_OS::write (this->poll_fd_,
                         pfd,
                         sizeof (pfd)) != sizeof (pfd))
        return -1;
#elif defined (ACE_HAS_EVENT_POLL)

      struct epoll_event epev;
      ACE_OS::memset (&epev, 0, sizeof (epev));
      int op;

      // ACE_Event_Handler::NULL_MASK ???
      if (new_mask == 0)
        {
          op          = EPOLL_CTL_DEL;
          epev.events = 0;
        }
      else
        {
          op          = EPOLL_CTL_MOD;
          epev.events = events;
        }

      epev.data.fd = handle;

      if (::epoll_ctl (this->poll_fd_, op, handle, &epev) == -1)
        {
          // If a handle is closed, epoll removes it from the poll set
          // automatically - we may not know about it yet. If that's the
          // case, a mod operation will fail with ENOENT. Retry it as
          // an add.
          if (op == EPOLL_CTL_MOD && errno == ENOENT &&
              ::epoll_ctl (this->poll_fd_, EPOLL_CTL_ADD, handle, &epev) == -1)
            return -1;
        }

#else
      pollfd pfd[1];

      pfd[0].fd      = handle;
      pfd[0].events  = events;
      pfd[0].revents = 0;

      // Change the events associated with the given file descriptor.
      if (ACE_OS::write (this->poll_fd_,
                         pfd,
                         sizeof (pfd)) != sizeof (pfd))
        return -1;
#endif /*ACE_HAS_EVENT_POLL  */
    }

  return old_mask;
}

int
ACE_Dev_Poll_Reactor::ready_ops (ACE_Event_Handler * /* event_handler */,
                                 ACE_Reactor_Mask /* mask */,
                                 int /* ops */)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::ready_ops");

  // Since the Dev_Poll_Reactor uses the poll result buffer, the
  // ready_set cannot be directly manipulated outside of the event
  // loop.
  ACE_NOTSUP_RETURN (-1);
}

int
ACE_Dev_Poll_Reactor::ready_ops (ACE_HANDLE /* handle */,
                                 ACE_Reactor_Mask /* mask */,
                                 int /* ops */)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::ready_ops");

  // Since the Dev_Poll_Reactor uses the poll result buffer, the
  // ready_set cannot be directly manipulated outside of the event
  // loop.
  ACE_NOTSUP_RETURN (-1);
}

void
ACE_Dev_Poll_Reactor::dump (void) const
{
#if defined (ACE_HAS_DUMP)
  ACE_TRACE ("ACE_Dev_Poll_Reactor::dump");

  ACE_DEBUG ((LM_DEBUG, ACE_BEGIN_DUMP, this));
  ACE_DEBUG ((LM_DEBUG, ACE_TEXT ("restart_ = %d\n"), this->restart_));
  ACE_DEBUG ((LM_DEBUG,
              ACE_TEXT ("initialized_ = %d"),
              this->initialized_));
  ACE_DEBUG ((LM_DEBUG, ACE_TEXT ("poll_fd_ = %d"), this->poll_fd_));
  ACE_DEBUG ((LM_DEBUG, ACE_TEXT ("size_ = %u"), this->size_));
  ACE_DEBUG ((LM_DEBUG,
              ACE_TEXT ("deactivated_ = %d"),
              this->deactivated_));
  ACE_DEBUG ((LM_DEBUG, ACE_END_DUMP));
#endif /* ACE_HAS_DUMP */
}

short
ACE_Dev_Poll_Reactor::reactor_mask_to_poll_event (ACE_Reactor_Mask mask)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::reactor_mask_to_poll_event");

  if (mask == ACE_Event_Handler::NULL_MASK)
    // No event.  Remove from interest set.
#if defined (ACE_HAS_EVENT_POLL)
    return EPOLL_CTL_DEL;
#else
    return POLLREMOVE;
#endif /* ACE_HAS_EVENT_POLL */

  short events = 0;

  // READ, ACCEPT, and CONNECT flag will place the handle in the
  // read set.
  if (ACE_BIT_ENABLED (mask, ACE_Event_Handler::READ_MASK)
      || ACE_BIT_ENABLED (mask, ACE_Event_Handler::ACCEPT_MASK)
      || ACE_BIT_ENABLED (mask, ACE_Event_Handler::CONNECT_MASK))
    {
#if defined (ACE_HAS_EVENT_POLL)
      ACE_SET_BITS (events, EPOLLIN);
#else
      ACE_SET_BITS (events, POLLIN);
#endif /*ACE_HAS_EVENT_POLL*/
    }

  // WRITE and CONNECT flag will place the handle in the write set.
  if (ACE_BIT_ENABLED (mask, ACE_Event_Handler::WRITE_MASK)
      || ACE_BIT_ENABLED (mask, ACE_Event_Handler::CONNECT_MASK))
    {
#if defined (ACE_HAS_EVENT_POLL)
      ACE_SET_BITS (events, EPOLLOUT);
#else
      ACE_SET_BITS (events, POLLOUT);
#endif /*ACE_HAS_EVENT_POLL*/
    }

  // EXCEPT flag will place the handle in the except set.
  if (ACE_BIT_ENABLED (mask, ACE_Event_Handler::EXCEPT_MASK))
    {
#if defined (ACE_HAS_EVENT_POLL)
      ACE_SET_BITS (events, EPOLLPRI);
#else
      ACE_SET_BITS (events, POLLPRI);
#endif /*ACE_HAS_EVENT_POLL*/
    }

  return events;
}

namespace {
  void polite_sleep_hook (void *) { }
}

int
ACE_Dev_Poll_Reactor::Token_Guard::acquire_quietly (ACE_Time_Value *max_wait)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::Token_Guard::acquire_quietly");

  // Acquire the token but don't ping any waiters; just queue up politely.
  int result = 0;
  if (max_wait)
    {
      ACE_Time_Value tv = ACE_OS::gettimeofday ();
      tv += *max_wait;

      ACE_MT (result = this->token_.acquire_read (&polite_sleep_hook,
                                                  0,
                                                  &tv));
    }
  else
    {
      ACE_MT (result = this->token_.acquire_read (&polite_sleep_hook));
    }

  // Check for timeouts and errors.
  if (result == -1)
    {
      if (errno == ETIME)
        return 0;
      else
        {
          ACE_ERROR ((LM_ERROR, "%t: %p\n", "token acquire_read"));
          return -1;
        }
    }

  // We got the token and so let us mark ourselves as owner
  this->owner_ = 1;

  return result;
}

int
ACE_Dev_Poll_Reactor::Token_Guard::acquire (ACE_Time_Value *max_wait)
{
  ACE_TRACE ("ACE_Dev_Poll_Reactor::Token_Guard::acquire");

  // Try to grab the token.  If someone if already there, don't wake
  // them up, just queue up in the thread pool.
  int result = 0;
  if (max_wait)
    {
      ACE_Time_Value tv = ACE_OS::gettimeofday ();
      tv += *max_wait;

      ACE_MT (result = this->token_.acquire (0, 0, &tv));
    }
  else
    {
      ACE_MT (result = this->token_.acquire ());
    }

  // Check for timeouts and errors.
  if (result == -1)
    {
      if (errno == ETIME)
        return 0;
      else
        return -1;
    }

  // We got the token and so let us mark ourseleves as owner
  this->owner_ = 1;

  return result;
}

ACE_END_VERSIONED_NAMESPACE_DECL

#endif  /* ACE_HAS_EVENT_POLL || ACE_HAS_DEV_POLL */
