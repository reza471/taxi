# Zephyr rewrite (work in progress)

- `core/` : portable C code, no OS calls. Fare engine is done and tested.
- `tests/`: host tests. Run:

      gcc -Wall -Wextra -O1 -o /tmp/test_fare core/fare_engine.c tests/test_fare.c && /tmp/test_fare

Next: Zephyr app (devicetree overlay, tasks, display, keys, network).
Fare rules: distance always charged (after free part); time charged only below `max_stop_kmh`;
night % applies to start fare, distance and time; night is decided at trip start.
