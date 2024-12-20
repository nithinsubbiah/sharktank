# Copyright 2024 Advanced Micro Devices, Inc.
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import logging
import multiprocessing
import os
from pathlib import Path
import pytest
import time
from unittest.mock import patch

pytest.importorskip("sglang")
from sglang import bench_serving

from .utils import SGLangBenchmarkArgs, log_jsonl_result

from integration_tests.llm.utils import (
    find_available_port,
    start_llm_server,
)

logger = logging.getLogger(__name__)

device_settings = {
    "device_flags": [
        "--iree-hal-target-backends=rocm",
        "--iree-hip-target=gfx942",
    ],
    "device": "hip",
}


@pytest.mark.parametrize(
    "request_rate,model_param_file_name",
    [
        (req_rate, "meta-llama-3.1-8b-instruct.f16.gguf")
        for req_rate in [1, 2, 4, 8, 16, 32]
    ],
)
@pytest.mark.parametrize(
    "pre_process_model",
    [
        (
            {
                "model_name": "llama3_8B_fp16",
                "model_param_file_name": "meta-llama-3.1-8b-instruct.f16.gguf",
                "settings": device_settings,
                "batch_sizes": [1, 4],
            }
        )
    ],
    indirect=True,
)
def test_sglang_benchmark_server(
    request_rate, model_param_file_name, pre_process_model
):
    # TODO: Remove when multi-device is fixed
    os.environ["ROCR_VISIBLE_DEVICES"] = "1"

    tmp_dir = pre_process_model

    config_path = tmp_dir / "config.json"
    vmfb_path = tmp_dir / "model.vmfb"
    tokenizer_path = tmp_dir / "tokenizer.json"
    model_path = tmp_dir / model_param_file_name

    # Start shortfin llm server
    port = find_available_port()
    server_process = start_llm_server(
        port,
        tokenizer_path,
        config_path,
        vmfb_path,
        model_path,
        device_settings,
        timeout=30,
    )

    # Run and collect SGLang Serving Benchmark
    benchmark_args = SGLangBenchmarkArgs(
        backend="shortfin",
        num_prompt=10,
        base_url=f"http://localhost:{port}",
        tokenizer=tmp_dir,
        request_rate=request_rate,
    )
    output_file = (
        tmp_dir
        / f"{benchmark_args.backend}_{benchmark_args.num_prompt}_{benchmark_args.request_rate}.jsonl"
    )
    benchmark_args.output_file = output_file

    logger.info("Running SGLang Benchmark with the following args:")
    logger.info(benchmark_args)
    try:
        start = time.time()
        with patch.object(bench_serving, "print", side_effect=logger.info):
            benchmark_process = multiprocessing.Process(
                target=bench_serving.run_benchmark,
                args=(benchmark_args.as_namespace(),),
            )
            benchmark_process.start()
            benchmark_process.join()

        logger.info(f"Benchmark run completed in {str(time.time() - start)} seconds")
        logger.info("======== RESULTS ========")
        log_jsonl_result(benchmark_args.output_file)
    except Exception as e:
        logger.error(e)

    server_process.terminate()
    server_process.wait()
