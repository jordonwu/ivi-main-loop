#include "glib/ivi-main-loop-glib.h"

#include "ivi-main-loop-log.h"
#include "ivi-main-loop-private.h"

#include "assert.h"

namespace ivi {

#ifdef USE_IVI_LOGGING
LOG_DECLARE_CONTEXT(iviMainLoopContext, "MAIN", "");
#endif

/**
 * g_io_add_watch_full:
 *
 * @param channel a GIOChannel
 * @param priority the priority of the GIOChannel source
 * @param condition the condition to watch for
 * @param func the function to call when the condition is satisfied
 * @param user_data user data to pass to @func
 * @param notify the function to call when the source is removed
 * @param context the GMainContext to use
 *
 * Adds the GIOChannel into the given main loop context
 * with the given priority.
 *
 * This internally creates a main loop source using g_io_create_watch()
 * and attaches it to the main loop context with g_source_attach().
 * You can do these steps manually if you need greater control.
 *
 * @return the event source id
 */
guint g_io_add_watch_full_with_context(GIOChannel *channel, gint priority, GIOCondition condition,
            GIOFunc func, gpointer user_data, GDestroyNotify notify, GMainContext *context)
{
    g_return_val_if_fail(channel != NULL, 0);
    GSource *source = g_io_create_watch(channel, condition);

    if (priority != G_PRIORITY_DEFAULT) {
        g_source_set_priority(source, priority);
    }
    g_source_set_callback(source, (GSourceFunc) func, user_data, notify);

    guint id = g_source_attach(source, context);
    g_source_unref(source);

    return id;
}

inline GIOCondition operator|(const GIOCondition c1, const GIOCondition c2)
{
    return static_cast<GIOCondition>(static_cast<int>(c1) | static_cast<int>(c2));
}

inline GIOCondition &operator|=(GIOCondition &c1, const GIOCondition c2)
{
    c1 = c1 | c2;
    return c1;
}

inline FileDescriptorWatchEventSource::Event operator|(const FileDescriptorWatchEventSource::Event c1,
            const FileDescriptorWatchEventSource::Event c2)
{
    return static_cast<FileDescriptorWatchEventSource::Event>(static_cast<int>(c1) | static_cast<int>(c2));
}

inline FileDescriptorWatchEventSource::Event &operator|=(FileDescriptorWatchEventSource::Event &c1,
            const FileDescriptorWatchEventSource::Event c2)
{
    c1 = c1 | c2;
    return c1;
}

GLibIdle::~GLibIdle()
{
    disable();
}

void GLibIdle::enable()
{
    if (m_source == nullptr) {
        m_source = g_idle_source_new();
        g_source_set_callback(m_source, &onGlibCallback, this, nullptr);
        g_source_attach(m_source, m_mainLoop.getGMainContext());
    }
}

void GLibIdle::disable()
{
    g_source_destroy(m_source);
    g_source_unref(m_source);
    m_source = nullptr;
}

gboolean GLibIdle::onGlibCallback(gpointer data)
{
    auto thiz = static_cast<GLibIdle *>(data);

    if (thiz->m_func() == ReportStatus::KEEP_ENABLED) {
        return true;
    } else {
        thiz->disable();
        return false;
    }
}

GLibTimeOut::~GLibTimeOut()
{
    disable();
}

void GLibTimeOut::setDuration(DurationInMilliseconds duration)
{
    disable();
    m_duration = duration;
    enable();
}

void GLibTimeOut::enable()
{
    if (m_source != nullptr) {
        disable();
    }

    m_source = g_timeout_source_new(getDuration());
    g_source_set_callback(m_source, onTimerCallback, this, nullptr);
    g_source_attach(m_source, m_mainLoop.getGMainContext());
}

void GLibTimeOut::disable()
{
    if (m_source != nullptr) {
        g_source_destroy(m_source);
        g_source_unref(m_source);
        m_source = nullptr;
    }
}

gboolean GLibTimeOut::onTimerCallback(gpointer data)
{
    auto thiz = static_cast<GLibTimeOut *>(data);
    ReportStatus status = thiz->m_func();

    if (status != ReportStatus::KEEP_ENABLED) {
        thiz->disable();
        return false;
    } else {
        return true;
    }

}

GLibFileDescriptorWatch::GLibFileDescriptorWatch(GlibEventDispatcher &mainLoop, CallBackFunction callBackFunction,
            FileDescriptor fileDescriptor,
            Event events) :
    FileDescriptorWatchEventSource(callBackFunction), m_mainLoop(mainLoop), m_events(events)
{
    m_channel = g_io_channel_unix_new(fileDescriptor);
}

GLibFileDescriptorWatch::~GLibFileDescriptorWatch()
{
    disable();
    if (m_channel != nullptr) {
        g_io_channel_unref(m_channel);
    }
}

void GLibFileDescriptorWatch::disable()
{
    if (inputSourceID != UNREGISTERED_SOURCE) {
        g_source_remove(inputSourceID);
    }
    inputSourceID = UNREGISTERED_SOURCE;
}

GLibFileDescriptorWatch::Event GLibFileDescriptorWatch::toEventSource(const GIOCondition condition)
{
    Event e = Event::NONE;

    if (hasFlag(condition, G_IO_IN)) {
        e |= Event::READ_AVAILABLE;
    }
    if (hasFlag(condition, G_IO_OUT)) {
        e |= Event::WRITE_AVAILABLE;
    }
    if (hasFlag(condition, G_IO_HUP)) {
        e |= Event::HANG_UP;
    }

    return e;
}

GIOCondition GLibFileDescriptorWatch::toGIOCondition(const Event event)
{
    GIOCondition condition = static_cast<GIOCondition>(0);
    if (hasFlag(event, Event::READ_AVAILABLE)) {
        condition |= G_IO_IN;
    }
    if (hasFlag(event, Event::WRITE_AVAILABLE)) {
        condition |= G_IO_OUT;
    }
    if (hasFlag(event, Event::HANG_UP)) {
        condition |= G_IO_HUP;
    }
    return condition;
}

void GLibFileDescriptorWatch::enable()
{
    if (!m_isEnabled) {

        inputSourceID = g_io_add_watch_full_with_context(m_channel, G_PRIORITY_DEFAULT, toGIOCondition(m_events),
                    onSocketDataAvailableGlibCallback, this, nullptr, m_mainLoop.getGMainContext());
        m_isEnabled = true;
    }
}

gboolean GLibFileDescriptorWatch::onSocketDataAvailableGlibCallback(GIOChannel *gio, GIOCondition condition, gpointer data)
{
    GLibFileDescriptorWatch *thiz = static_cast<GLibFileDescriptorWatch *>(data);
    Event event = toEventSource(condition);
    assert(event != Event::NONE);
    {
        ReportStatus status = thiz->m_callBack(event);

        if (status != ReportStatus::KEEP_ENABLED) {
            thiz->disable();
            return false;
        } else {
            return true;
        }
    }
}

GlibEventDispatcher::GlibEventDispatcher(GMainContext *context)
{
    m_context = context;
}

void GlibEventDispatcher::run()
{
    log_debug() << "run";

    m_mainLoop = g_main_loop_new(m_context, false);

    g_main_loop_run(m_mainLoop);

    g_main_loop_unref(m_mainLoop);
    m_mainLoop = nullptr;
}

void GlibEventDispatcher::quit()
{
    log_debug() << "quit";
    g_main_loop_quit(m_mainLoop);
}

IdleEventSource *GlibEventDispatcher::newIdleEventSource(const IdleEventSource::CallBackFunction &callBackFunction)
{
    return new GLibIdle(*this, callBackFunction);
}

TimeOutEventSource *GlibEventDispatcher::newTimeoutEventSource(const TimeOutEventSource::CallBackFunction &callBackFunction,
            DurationInMilliseconds duration)
{
    return new GLibTimeOut(*this, callBackFunction, duration);
}

FileDescriptorWatchEventSource *GlibEventDispatcher::newFileDescriptorWatchEventSource(
            const FileDescriptorWatchEventSource::CallBackFunction &callBackFunction, FileDescriptor fileDescriptor,
            FileDescriptorWatchEventSource::Event events)
{
    return new GLibFileDescriptorWatch(*this, callBackFunction, fileDescriptor, events);
}

}
