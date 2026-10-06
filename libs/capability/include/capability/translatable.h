#pragma once

// Qt lupdate recognizes this as a QT_TRANSLATE_NOOP alias. Policy remains
// independent of Qt, and frontend adapters translate only presentation fields.
#define EXOSNAP_TRANSLATABLE(context, source) source
