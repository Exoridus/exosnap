export default {
  multipass: true,
  floatPrecision: 4,
  plugins: [{
    name: 'preset-default',
    params: {
      overrides: {
        cleanupIds: {
          preservePrefixes: ['body', 'btn-', 'stick-', 'trigger-', 'trackpad-', 'dpad', 'home-', 'touchpad-'],
        },
      },
    },
  }],
};
