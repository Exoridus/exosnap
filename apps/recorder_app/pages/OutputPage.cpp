#include "OutputPage.h"
#if __has_include("pages/OutputPage.xaml.g.hpp")
#include "pages/OutputPage.xaml.g.hpp"
#endif

namespace winrt::exosnap::implementation {
OutputPage::OutputPage() {
    InitializeComponent();
}
} // namespace winrt::exosnap::implementation

