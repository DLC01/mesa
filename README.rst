`Mesa <https://mesa3d.org>`_ - The 3D Graphics Library
======================================================


Source
------

This repository lives at https://gitlab.freedesktop.org/mesa/mesa.
Other repositories are likely forks, and code found there is not supported.


Build & install
---------------

You can find more information in our documentation (`docs/install.rst
<https://mesa3d.org/install.html>`_), but the recommended way is to use
Meson (`docs/meson.rst <https://mesa3d.org/meson.html>`_):

.. code-block:: sh

  $ meson build -Dplatforms=x11,wayland -Dprefix=$PREFIX -Dcpp_rtti=false -Ddri3=enabled -Ddri-drivers=[] -Dgallium-drivers=swrast,zink -Dvulkan-drivers=[] -Degl=enabled -Dglx=dri -Dshared-glapi=enabled -Dshader-cache=enabled -Dgles1=enabled -Dgles2=enabled -Dllvm=enabled -Dshared-llvm=enabled -Dlmsensors=disabled -Dgbm=disabled -Dlibunwind=disabled -Dopencl-native=false -Dgallium-vdpau=disabled -Dgallium-xvmc=disabled -Dgallium-omx=disabled -Dgallium-va=disabled -Dgallium-xa=disabled -Dmicrosoft-clc=disabled -Dvalgrind=disabled -Dpower8=disabled
  $ ninja -C build install -j4


Support
-------

Many Mesa devs hang on IRC; if you're not sure which channel is
appropriate, you should ask your question on `OFTC's #dri-devel
<irc://irc.oftc.net/dri-devel>`_, someone will redirect you if
necessary.
Remember that not everyone is in the same timezone as you, so it might
take a while before someone qualified sees your question.
To figure out who you're talking to, or which nick to ping for your
question, check out `Who's Who on IRC
<https://dri.freedesktop.org/wiki/WhosWho/>`_.

The next best option is to ask your question in an email to the
mailing lists: `mesa-dev\@lists.freedesktop.org
<https://lists.freedesktop.org/mailman/listinfo/mesa-dev>`_


Bug reports
-----------

If you think something isn't working properly, please file a bug report
(`docs/bugs.rst <https://mesa3d.org/bugs.html>`_).


Contributing
------------

Contributions are welcome, and step-by-step instructions can be found in our
documentation (`docs/submittingpatches.rst
<https://mesa3d.org/submittingpatches.html>`_).

Note that Mesa uses gitlab for patches submission, review and discussions.
