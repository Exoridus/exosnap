using ExoSnap.Verify.Windows;

namespace ExoSnap.Verify.Tests;

/// <summary>The pure mapping half of the apps-colour appearance adapter.</summary>
public sealed class SystemAppearanceMappingTests
{
    [Fact]
    public void OneIsLight() => Assert.Equal(AppsAppearance.Light, WindowsSystemAppearance.FromRegistryValue(1));

    [Fact]
    public void ZeroIsDark() => Assert.Equal(AppsAppearance.Dark, WindowsSystemAppearance.FromRegistryValue(0));

    [Fact]
    public void AMissingValueIsUnknownNotAGuessedDefault() =>
        Assert.Equal(AppsAppearance.Unknown, WindowsSystemAppearance.FromRegistryValue(null));

    [Fact]
    public void ANonNumericValueIsUnknown() =>
        Assert.Equal(AppsAppearance.Unknown, WindowsSystemAppearance.FromRegistryValue("light"));
}
