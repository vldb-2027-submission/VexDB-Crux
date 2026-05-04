#!/usr/bin/env python3
import struct
import csv
import sys

def fbin_to_csv(fbin_path, csv_path, start_id):
    with open(fbin_path, "rb") as f:
        # Read header: num_vectors, vector_dim (uint32)
        header = f.read(8)
        if len(header) < 8:
            raise ValueError("Invalid fbin file: header too short")

        num_vectors, vector_dim = struct.unpack("II", header)
        print(f"[INFO] num_vectors = {num_vectors}, vector_dim = {vector_dim}")

        vector_bytes = vector_dim * 4  # float32 = 4 bytes

        with open(csv_path, "w", newline="") as csvfile:
            writer = csv.writer(csvfile)
            current_id = start_id

            for i in range(num_vectors):
                vec_data = f.read(vector_bytes)
                if len(vec_data) < vector_bytes:
                    raise ValueError(f"Unexpected EOF when reading vector {i}")

                vector = struct.unpack(f"{vector_dim}f", vec_data)

                vector_str = "[" + ",".join(f"{v}" for v in vector) + "]"
                writer.writerow([current_id, vector_str])

                current_id += 1

    print(f"[OK] Converted {num_vectors} vectors → {csv_path}")


def main():
    if len(sys.argv) != 4:
        print("Usage: python3 fbin_to_csv.py <input.fbin> <output.csv> <start_id>")
        sys.exit(1)

    fbin_path = sys.argv[1]
    csv_path = sys.argv[2]
    start_id = int(sys.argv[3])

    fbin_to_csv(fbin_path, csv_path, start_id)


if __name__ == "__main__":
    main()