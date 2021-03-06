from scannerpy import Database, Job
import os.path

################################################################################
# This tutorial shows how to write and use your own custom op.                 #
################################################################################

# Look at resize_op/resize_op.cpp to start this tutorial.

with Database() as db:

    if not os.path.isfile('resize_op/build/libresize_op.so'):
        print('You need to build the custom op first: \n'
              '$ cd resize_op; mkdir build && cd build; cmake ..; make')
        exit()

    # To load a custom op into the Scanner runtime, we use db.load_op to open the
    # shared library we compiled. If the op takes arguments, it also optionally
    # takes a path to the generated python file for the arg protobuf.
    db.load_op('resize_op/build/libresize_op.so', 'resize_op/build/resize_pb2.py')

    frame, frame_info = db.table('example').as_op().all()

    # Then we use our op just like in the other examples.
    resize = db.ops.Resize(
        frame = frame, frame_info = frame_info,
        width = 200, height = 300)

    job = Job(columns = [resize, frame_info], name = 'example_resized')
    db.run(job, force=True)
