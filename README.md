# QNXHACK-CHN-008
the city digital twin synchronization engine using QNX OS and Raspberry Pi 5 
If you have placed all the `.c`, `.h`, `.py`, and `.html` files into a single, flat directory (without any folders), standard Makefiles with folder paths will fail.

Here are the direct, copy-pasteable commands to compile and run the system from a single flat folder.

### 1. Start the Hardware Bridge (Windows Host)

Open a terminal in the folder containing your files and run the Python bridge to connect the Arduino:

```bash
pip install pyserial
python laptop_serial_tcp_bridge.py

```

### 2. Compile the QNX Binaries (QNX Target)

Transfer all files to your Raspberry Pi 5. Open the QNX terminal, navigate to that flat folder, and run these direct `qcc` (QNX C Compiler) commands. The necessary POSIX libraries (`-lpthread`, `-lrt` for shared memory, `-lm` for math, `-lsocket` for networking) are explicitly linked:

```bash
# 1. Compile Core Simulators & Timers
qcc -o sensor_simulator sensor_simulator.c -lpthread -lrt
qcc -o weather_simulator weather_simulator.c -lpthread -lrt
qcc -o upstream_simulator upstream_simulator.c -lm -lpthread -lrt
qcc -o rtos_timer_engine rtos_timer_engine.c -lpthread -lrt

# 2. Compile Data Fusion Engine & Sensor Gateway
qcc -o arduino_sensor arduino_sensor.c -lsocket
qcc -o reservoir_engine reservoir_engine.c -lpthread -lrt

# 3. Compile IPC Operators & Network Bridge
qcc -o operator_server operator_server.c
qcc -o operator_client operator_client.c
qcc -o ws_bridge ws_bridge.c -lpthread -lsocket

# 4. Compile the High Availability Watchdog Supervisor
qcc -o watchdog watchdog.c

# 5. Compile the Isolated Demos (Optional)
qcc -o shared_memory_demo shared_memory_demo.c -lpthread -lrt
qcc -o sync_demo sync_demo.c -lpthread
qcc -o one_shot_timer_demo one_shot_timer_demo.c -lpthread -lrt

```

### 3. Launch the System (QNX Target)

Because the files are now in the same directory, you can simply launch the supervisor process directly. The watchdog will automatically spawn the rest of the executables:

```bash
./watchdog

```

### 4. Monitor & Interact

* **Visual Dashboard:** Double-click `dashboard.html` to open it in any web browser. It will automatically connect to the QNX target to visualize the real-time twin.
* **Command Line IPC:** To manually request a synchronous state snapshot, open a second terminal window on the QNX target and run:
```bash
./operator_client

```
