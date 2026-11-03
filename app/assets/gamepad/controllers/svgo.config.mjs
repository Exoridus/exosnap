export default {
  multipass: true,
  floatPrecision: 4,
  plugins: [{
    name: 'preset-default',
    params: {
      overrides: {
        // Mapping references include invisible pressure and knockout shapes.
        cleanupIds: false,
        removeHiddenElems: false,
      },
    },
  }],
};
