i want to improve the security of the cluster:

- the name in swim_start_opts_t is a secret known to all nodes
- in every udp packet, we need to add 12 bytes 
	- tval: 4 byte, current time on sender
	- hval: 8 byte, hash of (time, sender->name)
- when receiving a udp packet:
	- make sure that tval is current time of receiver +- 10s
	- make sure that hval matches hash(time, receiver-name)

/* Copyright (c) 2026, CK Tan.
 * https://github.com/cktan/swimc17/blob/main/LICENSE
 */
