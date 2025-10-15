# Minimal CAN / CAN FD Setup with can-utils

This guide sets up a virtual CAN interface (`vcan0`) and allows you to send/receive frames using `can-utils`.

---

## 1️⃣ Install can-utils

```
sudo apt update
sudo apt install can-utils
```

---

## 2️⃣ Load the virtual CAN kernel module

```
sudo modprobe vcan
```

---

## 3️⃣ Create a virtual CAN interface

```
sudo ip link add dev vcan0 type vcan
sudo ip link set up vcan0
```

Check that it is up:

```
ip -details link show vcan0
```

You should see `state UP` and `CAN 0x0` for virtual CAN.

---

## 4️⃣ Monitor the CAN bus

Open a terminal and run:

```
candump vcan0
```

This will show all frames received on `vcan0`.

---

## 5️⃣ Send a CAN frame

Classic CAN (up to 8 bytes):

```
cansend vcan0 123#1122334455667788
```

CAN FD frame (up to 64 bytes, BRS/FDF):

```
cansend vcan0 123##0A112233445566778899AABBCCDDEEFF
```

- `123` → CAN ID  
- `#` → Classic CAN separator  
- `##0A` → CAN FD frame with default flags (FDF+BRS)  
- Data bytes → hex string  

---

## 6️⃣ Optional: Send continuously

Use `cangen` to generate traffic:

```
cangen vcan0 -g 1000 -I 123 -D 8
```

- `-g 1000` → interval in ms  
- `-I 123` → CAN ID  
- `-D 8` → DLC (data length)

---

## 7️⃣ Cleanup

```
sudo ip link delete vcan0
```

---

✅ You now have a fully working virtual CAN environment ready to test the GVSOC CAN model.
