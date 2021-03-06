#include "quill/handlers/Handler.h"

#include "quill/filters/LogLevelFilter.h"
#include <algorithm>
#include <mutex>

namespace quill
{

/***/
void Handler::set_log_level(LogLevel log_level)
{
  std::lock_guard<detail::RecursiveSpinlock> const lock(_global_filters_lock);
  auto search_log_level_filter_it =
    std::find_if(_global_filters.begin(), _global_filters.end(), [](std::unique_ptr<FilterBase>& filter) {
      return filter->get_filter_name() == LogLevelFilter::filter_name;
    });

  if (search_log_level_filter_it != _global_filters.end())
  {
    // Just update the existing
    auto* log_level_filter = reinterpret_cast<LogLevelFilter*>(search_log_level_filter_it->get());
    log_level_filter->set_log_level(log_level);
  }
  else
  {
    // construct a new filter and add it to global
    auto log_level_filter = std::make_unique<LogLevelFilter>(log_level);
    add_filter(std::move(log_level_filter));
  }
}

/***/
QUILL_NODISCARD LogLevel Handler::get_log_level() noexcept
{
  // When set_log_level is called for the first time, it won't exist in _local_filters
  // until apply_filters has run at least once

  auto const search_log_level_filter_it =
    std::find_if(_local_filters.cbegin(), _local_filters.cend(), [](FilterBase const* filter) {
      return filter->get_filter_name() == LogLevelFilter::filter_name;
    });

  if (search_log_level_filter_it != _local_filters.cend())
  {
    return reinterpret_cast<LogLevelFilter*>(*search_log_level_filter_it)->get_log_level();
  }
  else
  {
    // No filter found
    return LogLevel::TraceL3;
  }
}

/***/
void Handler::add_filter(std::unique_ptr<FilterBase> filter)
{
  // Lock and add this filter to our global collection
  std::lock_guard<detail::RecursiveSpinlock> const lock(_global_filters_lock);

  // Check if the same filter already exists
  auto const search_filter_it = std::find_if(
    _global_filters.cbegin(), _global_filters.cend(), [&filter](std::unique_ptr<FilterBase> const& elem_filter) {
      return elem_filter->get_filter_name() == filter->get_filter_name();
    });

  if (QUILL_UNLIKELY(search_filter_it != _global_filters.cend()))
  {
    QUILL_THROW(QuillError{"Filter with the same name already exists"});
  }

  _global_filters.push_back(std::move(filter));

  // Indicate a new filter was added - here relaxed is okay as the spinlock will do acq-rel on destruction
  _new_filter.store(true, std::memory_order_relaxed);
}

/***/
QUILL_NODISCARD bool Handler::apply_filters(char const* thread_id, std::chrono::nanoseconds log_record_timestamp,
                                            detail::LogRecordMetadata const& metadata,
                                            fmt::memory_buffer const& formatted_record)
{
  // Update our local collection of the filters
  if (QUILL_UNLIKELY(_new_filter.load(std::memory_order_relaxed)))
  {
    // if there is a new filter we have to update
    _local_filters.clear();

    std::lock_guard<detail::RecursiveSpinlock> const lock(_global_filters_lock);
    for (auto const& filter : _global_filters)
    {
      _local_filters.push_back(filter.get());
    }

    // all filters loaded so change to false
    _new_filter.store(false, std::memory_order_relaxed);
  }

  return std::all_of(
    _local_filters.begin(), _local_filters.end(),
    [thread_id, log_record_timestamp, &metadata, &formatted_record](FilterBase* filter_elem) {
      return filter_elem->filter(thread_id, log_record_timestamp, metadata, formatted_record);
    });
}

} // namespace quill