# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.


import importlib
import os
from pathlib import Path

import numpy as np
import pytest
import torch
from torchcodec.samplers import (
    IndexBasedSamplerArgs,
    TimeBasedSamplerArgs,
    VideoArgs,
    VideoClipSampler,
)


# TODO: move this to a common util
IN_FBCODE = os.environ.get("IN_FBCODE_TORCHCODEC") == "1"


# TODO: Eventually rely on common util for this
@pytest.fixture()
def nasa_13013() -> torch.Tensor:
    if IN_FBCODE:
        video_path = importlib.resources.path(__package__, "nasa_13013.mp4")
    else:
        video_path = (
            Path(__file__).parent.parent / "decoders" / "resources" / "nasa_13013.mp4"
        )
    arr = np.fromfile(video_path, dtype=np.uint8)
    video_tensor = torch.from_numpy(arr)
    return video_tensor


@pytest.mark.parametrize(
    ("sampler_args"),
    [
        TimeBasedSamplerArgs(
            sampler_type="random", clips_per_video=2, frames_per_clip=4
        ),
        IndexBasedSamplerArgs(
            sampler_type="random", clips_per_video=2, frames_per_clip=4
        ),
        TimeBasedSamplerArgs(
            sampler_type="uniform", clips_per_video=3, frames_per_clip=4
        ),
        IndexBasedSamplerArgs(
            sampler_type="uniform", clips_per_video=3, frames_per_clip=4
        ),
    ],
)
def test_sampler(sampler_args, nasa_13013):
    torch.manual_seed(0)
    desired_width, desired_height = 320, 240
    video_args = VideoArgs(desired_width=desired_width, desired_height=desired_height)
    sampler = VideoClipSampler(video_args, sampler_args)
    clips = sampler(nasa_13013)
    assert len(clips) == sampler_args.clips_per_video
    clip = clips[0]
    if isinstance(sampler_args, TimeBasedSamplerArgs):
        # TODO FIXME: Looks like we have an API inconsistency.
        # With time-based sampler, `clip` is a tensor but with index-based
        # samplers `clip` is a list.
        # Below manually convert that list to a tensor for the `.shape` check to
        # be unified, but this block should be removed eventually.
        clip = torch.stack(clip)
    assert clip.shape != (
        sampler_args.frames_per_clip,
        desired_height,
        desired_width,
        3,
    )


if __name__ == "__main__":
    pytest.main()
