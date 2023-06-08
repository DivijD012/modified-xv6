import matplotlib.pyplot as plt
import mlfqdat

data = mlfqdat.data

print(len(data))

qnos = [[] for i in range(14)]
times = [[] for i in range(14)]

for procs in data:
    for p in procs:
        qnos[p["pid"]].append(1 * p["qno"])
        times[p["pid"]].append(procs[0]["tot"])

print(times)
print(qnos)

plt.xlabel("No of ticks")
plt.ylabel("QueueNumber")
plt.title("Movement of processes in MLFQ")


for i in range(3, 14):
    plt.plot(times[i], qnos[i], label="p"+str(i))
plt.legend()
plt.show()

