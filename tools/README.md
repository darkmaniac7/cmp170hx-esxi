# tools

Read-only probes and verifiers used in the writeup. None of them writes to the GPU except where stated.

- `fuse_readout.py`: dumps the GA100 fuse/override registers (`0x820040`, `0x820084`, `0x820228`, `0x820250`, `0x820580`, `0x82057c`, `0x820520`, `0x823814` and friends) through a read-only BAR0 mapping. Run as root with the driver unloaded or on an idle card.
- `ecc_verify.py`: reads the ECC-related registers (`0x823814`, `0x9a0470`, the `0x8238xx` override block), NVML's ECC mode, and optionally runs the 60 GiB two-pattern test through `cmp580_trial_probe.py` (`--pattern`). `--diff a.json b.json` compares two captures (for example a control card and a recipe card on the same boot).
- `cmp580_trial_probe.py`: the CUDA probe. `--mode small` does arithmetic, `--mode vram --gib 60` allocates, writes and verifies two address-dependent patterns in bounded chunks. Needs a Python with torch and CUDA (`CUDA_PYTHON` env for `ecc_verify.py`).
- `host_link_state.py`: decodes the PCIe capability (LnkCap/LnkCap2/LnkSta/LnkCtl2) of the cards and their root ports from an ESXi-side `lspci` hex dump, fetched through `esxi_link_probe.py`.
- `esxi_link_probe.py`: logs into ESXi over the API, starts the `TSM-SSH` service if it was stopped, runs the given read-only commands over SSH, and stops the service again. Host, user and password come from flags/env; see the header.
- `cmp_pcie_speed.c`: asks the NVIDIA RM to query (0) or change (1/2/3) the PCIe link speed. Query is safe. Do not request 3 on an 8 GB 20c2; see README section 3.
