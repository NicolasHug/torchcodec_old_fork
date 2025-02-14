import os

import cv2
import torchcodec

decoder = torchcodec.decoders.core.create_from_file(
    os.path.dirname(__file__) + "/resources/nasa_13013.mp4"
)
torchcodec.decoders.core.add_video_stream(decoder, stream_index=3)
frame = torchcodec.decoders.core.get_frame_at_index(
    decoder, stream_index=3, frame_index=180
)

cv2.imwrite("frame180.png", frame.numpy())
