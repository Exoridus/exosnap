#include "EditDetailsRail.h"

namespace exosnap::ui::widgets {

EditDetailsRail::EditDetailsRail(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("editDetailsRail"));
}

void EditDetailsRail::setFacts(const Facts& /*facts*/) {
}

void EditDetailsRail::applyThemeStyles() {
}

} // namespace exosnap::ui::widgets
