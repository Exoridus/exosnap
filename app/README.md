# ExoSnap application layer

`app/` contains the Qt Quick frontend and the services/adapters connecting it to the recording engine. It is not another engine implementation.

Process-level startup/shutdown belongs to `bootstrap`. `quick/ExoSnap/Quick` owns the Quick application, typed adapters, scene-graph presentation and QML components. Shared models/services own configuration, recording coordination, source selection and persistence. Diagnostics, notifications and observability expose structured facts to UI, logs and verification clients.

The main application uses Qt Widgets for native tray integration only. The updater is a separate executable with its own Widgets UI. Do not introduce a second bootstrap, recreate a native child preview window or put capability/recording policy in QML.

See [Frontend architecture](../docs/architecture/frontend.md), [product specification](../docs/product-spec.md) and [build/test workflow](../docs/dev/build-and-test.md). Tests live alongside the application and Quick modules and are registered through the repository test helpers. `scripts/run-tests.ps1` provides isolation and a current-build receipt; a direct unqualified CTest run is not equivalent evidence.
