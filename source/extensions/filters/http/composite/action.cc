#include "source/extensions/filters/http/composite/action.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Composite {
void ExecuteFilterAction::createFilters(Http::FilterChainFactoryCallbacks& callbacks) const {
  cb_(callbacks);
}

void ExecuteFilterMultiActions::createMultiFilters(Http::FilterChainFactoryCallbacks& callbacks) const {
  for (auto cb : callbacks_) {
    cb(callbacks);
  }
}

REGISTER_FACTORY(ExecuteFilterActionFactory,
                 Matcher::ActionFactory<Http::Matching::HttpFilterActionContext>);
REGISTER_FACTORY(ExecuteFilterMultiActionFactory,
                 Matcher::ActionFactory<Http::Matching::HttpFilterActionContext>);
} // namespace Composite
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
