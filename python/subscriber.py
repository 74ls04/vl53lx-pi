import zmq
from multiprocessing import Queue
from queue import Empty
from threading import Thread

port = "5556"
ip = "127.0.0.1"  # localhost
address = "tcp://%s:%s" % (ip, port)
data_queue = Queue()


class Sensor(Thread):
    def __init__(self, address, queue):
        super().__init__()
        self._queue = queue
        context = zmq.Context()
        self.socket = context.socket(zmq.SUB)
        self.socket.connect(address)
        self.socket.set(zmq.SUBSCRIBE, b"")
        self.daemon = True
        self.start()

    def run(self) -> None:
        while True:
            try:
                payload = self.socket.recv_string(zmq.DONTWAIT)
                # payload = self.socket.recv_string()
                if payload:
                    self._queue.put(payload)

            except zmq.ZMQError as e:
                if e.errno == zmq.EAGAIN:
                    pass
                else:
                    print(e)

    def join(self) -> None:
        super().join()


def parse_packet(packet):
    packet_list = str(packet).split(" ")

    # Remove empty strings
    packet_list = [x for x in packet_list if x != ""]

    if len(packet_list) < 4:
        None

    measurement = {}
    measurement["count"] = int(packet_list[0])

    histogram = packet_list[1].split(",")
    histogram = [int(i) for i in histogram]
    # zero padd the histogram data to 23 bins
    histogram = histogram + [0] * (23 - len(histogram))
    measurement["histogram"] = histogram

    objects = packet_list[2:]
    measurement["objects"] = []
    for i in range(len(objects)):
        data = objects[i].split(",")
        measurement["objects"].append(
            {
                "status": int(data[0]),
                "min_range": int(data[1]),
                "range": int(data[2]),
                "max_range": int(data[3]),
                "sigma": float(data[4]),
                "signal_rate": float(data[5]),
                "amplitude_rate": float(data[6]),
            }
        )
    return measurement


def get_measurement():

    try:
        packet = data_queue.get(False)
        measurement = parse_packet(packet)
    except Empty:
        return

    return measurement


if __name__ == "__main__":

    sensor = Sensor(address, data_queue)

    while True:
        measurement = get_measurement()
        if measurement:
            print(measurement)
            print("\n")
