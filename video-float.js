(() => {
  const MIN_WIDTH = 240;
  const MIN_HEIGHT = 135;
  const MAX_VISIBLE_LARGE_VIDEOS = 3;
  const SCAN_INTERVAL_MS = 50;
  const BUTTON_GAP = 8;
  const HOVER_LEFT = 150;
  const HOVER_RIGHT = 24;
  const HOVER_ABOVE = 58;
  const HOVER_BELOW = 56;

  let running = false;
  let hasAnyVideo = false;
  let activeVideo = null;
  let activeRect = null;
  let button = null;
  let buttonVisible = false;
  let pointerTracking = false;
  let lastPointer = null;
  let pointerOnButton = false;
  let scanFrame = 0;
  let scanTimer = 0;
  let lastScanAt = 0;
  let hoverFrame = 0;
  let mutationObserver = null;
  let resizeObserver = null;
  let activeAttributeObserver = null;

  function inspectVideo(video) {
    const rect =
      video.getBoundingClientRect();

    if (
      rect.right <= 0 ||
      rect.bottom <= 0 ||
      rect.left >= window.innerWidth ||
      rect.top >= window.innerHeight
    ) {
      return null;
    }

    const visibleWidth =
      Math.max(
        0,
        Math.min(
          rect.right,
          window.innerWidth
        ) -
          Math.max(rect.left, 0)
      );

    const visibleHeight =
      Math.max(
        0,
        Math.min(
          rect.bottom,
          window.innerHeight
        ) -
          Math.max(rect.top, 0)
      );

    /*
     * "Large video" means the portion that is
     * actually visible in the current viewport.
     * A huge off-screen/preloaded video therefore
     * does not count toward the page limit.
     */
    if (
      visibleWidth < MIN_WIDTH ||
      visibleHeight < MIN_HEIGHT
    ) {
      return null;
    }

    const style =
      window.getComputedStyle(video);

    if (
      style.display === "none" ||
      style.visibility === "hidden" ||
      Number.parseFloat(
        style.opacity || "1"
      ) <= 0
    ) {
      return null;
    }

    return {
      video,
      rect,
      area:
        visibleWidth *
        visibleHeight
    };
  }

  function findPrimaryVideo() {
    const videos =
      document.getElementsByTagName(
        "video"
      );

    hasAnyVideo =
      videos.length > 0;

    let best = null;
    let visibleLargeCount = 0;

    for (const video of videos) {
      const candidate =
        inspectVideo(video);

      if (!candidate) {
        continue;
      }

      visibleLargeCount += 1;

      /*
       * PopTab targets pages with one obvious main
       * player. If four large videos are visible at
       * once, stop immediately instead of spending
       * more work trying to rank a video feed.
       */
      if (
        visibleLargeCount >
        MAX_VISIBLE_LARGE_VIDEOS
      ) {
        return null;
      }

      if (
        !best ||
        candidate.area >
          best.area
      ) {
        best = candidate;
      }
    }

    return best;
  }

  function ensureButton() {
    if (button) {
      return button;
    }

    button =
      document.createElement("button");

    button.type = "button";
    button.textContent = "浮窗";
    button.setAttribute(
      "aria-label",
      "用 NotF11 打开浮窗"
    );

    Object.assign(
      button.style,
      {
        all: "initial",
        position: "fixed",
        zIndex: "2147483647",
        display: "block",
        boxSizing: "border-box",
        padding: "5px 10px",
        border:
          "1px solid rgba(255,255,255,.18)",
        borderRadius: "7px",
        background:
          "rgba(24,24,27,.88)",
        color: "#fff",
        font:
          "12px/1.2 -apple-system,BlinkMacSystemFont,'Segoe UI','Microsoft YaHei',sans-serif",
        cursor: "pointer",
        boxShadow:
          "0 2px 10px rgba(0,0,0,.25)",
        backdropFilter:
          "blur(8px)",
        WebkitBackdropFilter:
          "blur(8px)",
        opacity: "0",
        transform:
          "translateY(3px)",
        transition:
          "opacity 120ms ease, transform 120ms ease",
        pointerEvents: "none",
        userSelect: "none"
      }
    );

    button.addEventListener(
      "click",
      (event) => {
        event.preventDefault();
        event.stopPropagation();

        button.disabled = true;

        chrome.runtime.sendMessage(
          {
            type:
              "notf11-open-video-float"
          },
          () => {
            if (button) {
              button.disabled = false;
            }

            if (
              chrome.runtime.lastError
            ) {
              console.debug(
                "NotF11 video float:",
                chrome.runtime
                  .lastError.message
              );
            }
          }
        );
      },
      true
    );

    document.documentElement
      .appendChild(button);

    return button;
  }

  function setButtonVisible(
    visible
  ) {
    if (
      !button ||
      buttonVisible === visible
    ) {
      return;
    }

    buttonVisible = visible;

    if (visible) {
      button.style.opacity = "1";
      button.style.transform =
        "translateY(0)";
      button.style.pointerEvents =
        "auto";
    } else {
      button.style.opacity = "0";
      button.style.transform =
        "translateY(3px)";
      button.style.pointerEvents =
        "none";
    }
  }

  function positionButton(
    rect = activeRect
  ) {
    if (
      !activeVideo ||
      !rect ||
      !button
    ) {
      return;
    }

    activeRect = rect;

    const width =
      button.offsetWidth;
    const height =
      button.offsetHeight;

    const left =
      Math.min(
        window.innerWidth -
          width -
          8,
        Math.max(
          8,
          rect.right - width
        )
      );

    const top =
      Math.max(
        8,
        rect.top -
          height -
          BUTTON_GAP
      );

    const nextLeft =
      `${Math.round(left)}px`;
    const nextTop =
      `${Math.round(top)}px`;

    if (
      button.style.left !==
      nextLeft
    ) {
      button.style.left =
        nextLeft;
    }

    if (
      button.style.top !==
      nextTop
    ) {
      button.style.top =
        nextTop;
    }
  }

  function pointerNearVideoCorner(
    x,
    y
  ) {
    const rect = activeRect;

    if (!rect) {
      return false;
    }

    return (
      x >=
        rect.right - HOVER_LEFT &&
      x <=
        rect.right + HOVER_RIGHT &&
      y >=
        rect.top - HOVER_ABOVE &&
      y <=
        rect.top + HOVER_BELOW
    );
  }

  function refreshHoverState() {
    if (
      !running ||
      !lastPointer ||
      !activeVideo ||
      document.fullscreenElement
    ) {
      setButtonVisible(false);
      return;
    }

    /*
     * Layout can move a video without changing the
     * video element itself. Refresh only the active
     * video's rect here; do not rescan every video on
     * every pointer frame.
     */
    const rect =
      activeVideo.getBoundingClientRect();

    const visibleWidth =
      Math.max(
        0,
        Math.min(
          rect.right,
          window.innerWidth
        ) -
          Math.max(rect.left, 0)
      );

    const visibleHeight =
      Math.max(
        0,
        Math.min(
          rect.bottom,
          window.innerHeight
        ) -
          Math.max(rect.top, 0)
      );

    if (
      visibleWidth < MIN_WIDTH ||
      visibleHeight < MIN_HEIGHT
    ) {
      activeRect = rect;
      setButtonVisible(false);
      return;
    }

    activeRect = rect;
    positionButton(rect);

    setButtonVisible(
      pointerOnButton ||
        pointerNearVideoCorner(
          lastPointer.x,
          lastPointer.y
        )
    );
  }

  function scheduleHoverRefresh() {
    if (
      !running ||
      hoverFrame
    ) {
      return;
    }

    hoverFrame =
      requestAnimationFrame(
        () => {
          hoverFrame = 0;
          refreshHoverState();
        }
      );
  }

  function onPointerMove(event) {
    lastPointer = {
      x: event.clientX,
      y: event.clientY
    };

    pointerOnButton =
      event.target === button;

    scheduleHoverRefresh();
  }

  function onPointerLeave() {
    lastPointer = null;
    pointerOnButton = false;
    setButtonVisible(false);
  }

  function setPointerTracking(
    enabled
  ) {
    if (
      pointerTracking === enabled
    ) {
      return;
    }

    pointerTracking = enabled;

    if (enabled) {
      document.addEventListener(
        "pointermove",
        onPointerMove,
        {
          capture: true,
          passive: true
        }
      );

      document.addEventListener(
        "pointerleave",
        onPointerLeave,
        {
          capture: true,
          passive: true
        }
      );
    } else {
      document.removeEventListener(
        "pointermove",
        onPointerMove,
        true
      );

      document.removeEventListener(
        "pointerleave",
        onPointerLeave,
        true
      );

      lastPointer = null;
      pointerOnButton = false;
    }
  }

  function disconnectVideoObservers() {
    if (resizeObserver) {
      resizeObserver.disconnect();
      resizeObserver = null;
    }

    if (
      activeAttributeObserver
    ) {
      activeAttributeObserver
        .disconnect();

      activeAttributeObserver =
        null;
    }
  }

  function observeActiveVideo(
    candidate
  ) {
    disconnectVideoObservers();

    activeVideo =
      candidate?.video ?? null;

    activeRect =
      candidate?.rect ?? null;

    if (!activeVideo) {
      setPointerTracking(false);
      setButtonVisible(false);
      return;
    }

    ensureButton();
    setPointerTracking(true);

    resizeObserver =
      new ResizeObserver(
        scheduleScan
      );

    resizeObserver.observe(
      activeVideo
    );

    activeAttributeObserver =
      new MutationObserver(
        scheduleScan
      );

    activeAttributeObserver.observe(
      activeVideo,
      {
        attributes: true,
        attributeFilter: [
          "class",
          "style",
          "hidden"
        ]
      }
    );

    positionButton(activeRect);
    refreshHoverState();
  }

  function scan() {
    scanFrame = 0;

    if (!running) {
      return;
    }

    const candidate =
      findPrimaryVideo();

    if (
      candidate?.video !==
      activeVideo
    ) {
      observeActiveVideo(
        candidate
      );
      return;
    }

    activeRect =
      candidate?.rect ?? null;

    if (activeVideo) {
      positionButton(
        activeRect
      );
    }

    refreshHoverState();
  }

  function scheduleScan() {
    if (
      !running ||
      scanFrame ||
      scanTimer
    ) {
      return;
    }

    const elapsed =
      performance.now() -
      lastScanAt;

    const delay =
      Math.max(
        0,
        SCAN_INTERVAL_MS -
          elapsed
      );

    const queueFrame = () => {
      scanTimer = 0;

      if (
        !running ||
        scanFrame
      ) {
        return;
      }

      scanFrame =
        requestAnimationFrame(
          () => {
            lastScanAt =
              performance.now();
            scan();
          }
        );
    };

    if (delay === 0) {
      queueFrame();
    } else {
      scanTimer =
        window.setTimeout(
          queueFrame,
          delay
        );
    }
  }

  function nodeContainsVideo(
    node
  ) {
    if (
      node.nodeType !==
      Node.ELEMENT_NODE
    ) {
      return false;
    }

    return (
      node.tagName === "VIDEO" ||
      Boolean(
        node.querySelector?.(
          "video"
        )
      )
    );
  }

  function onMutations(records) {
    if (
      activeVideo &&
      !activeVideo.isConnected
    ) {
      scheduleScan();
      return;
    }

    for (const record of records) {
      for (
        const node of
        record.addedNodes
      ) {
        if (
          nodeContainsVideo(node)
        ) {
          scheduleScan();
          return;
        }
      }

      /*
       * Removed subtrees do not need a recursive
       * video search. The active-video disconnect
       * check above already covers the only removal
       * that can invalidate the current selection.
       */
    }
  }

  function onScroll() {
    if (
      activeVideo ||
      hasAnyVideo
    ) {
      scheduleScan();
    }
  }

  function onResize() {
    if (
      activeVideo ||
      hasAnyVideo
    ) {
      scheduleScan();
    }
  }

  function onFullscreenChange() {
    if (
      document.fullscreenElement
    ) {
      setButtonVisible(false);
      return;
    }

    if (
      activeVideo ||
      hasAnyVideo
    ) {
      scheduleScan();
    }
  }

  function onMediaEvent(event) {
    if (
      event.target instanceof
        HTMLVideoElement
    ) {
      scheduleScan();
    }
  }

  function start() {
    if (running) {
      return;
    }

    running = true;

    mutationObserver =
      new MutationObserver(
        onMutations
      );

    mutationObserver.observe(
      document.documentElement,
      {
        childList: true,
        subtree: true
      }
    );

    window.addEventListener(
      "scroll",
      onScroll,
      {
        capture: true,
        passive: true
      }
    );

    window.addEventListener(
      "resize",
      onResize,
      {
        passive: true
      }
    );

    document.addEventListener(
      "fullscreenchange",
      onFullscreenChange
    );

    document.addEventListener(
      "play",
      onMediaEvent,
      true
    );

    document.addEventListener(
      "loadedmetadata",
      onMediaEvent,
      true
    );

    scheduleScan();
  }

  function stop() {
    if (!running) {
      return;
    }

    running = false;

    if (scanFrame) {
      cancelAnimationFrame(
        scanFrame
      );
      scanFrame = 0;
    }

    if (scanTimer) {
      clearTimeout(scanTimer);
      scanTimer = 0;
    }

    lastScanAt = 0;

    if (hoverFrame) {
      cancelAnimationFrame(
        hoverFrame
      );
      hoverFrame = 0;
    }

    mutationObserver?.disconnect();
    mutationObserver = null;

    disconnectVideoObservers();
    setPointerTracking(false);

    window.removeEventListener(
      "scroll",
      onScroll,
      true
    );

    window.removeEventListener(
      "resize",
      onResize
    );

    document.removeEventListener(
      "fullscreenchange",
      onFullscreenChange
    );

    document.removeEventListener(
      "play",
      onMediaEvent,
      true
    );

    document.removeEventListener(
      "loadedmetadata",
      onMediaEvent,
      true
    );

    activeVideo = null;
    activeRect = null;
    hasAnyVideo = false;
    lastPointer = null;
    pointerOnButton = false;
    buttonVisible = false;

    if (button) {
      button.remove();
      button = null;
    }
  }

  chrome.runtime.onMessage
    .addListener(
      (message) => {
        if (
          message?.type !==
          "notf11-video-float-enabled"
        ) {
          return;
        }

        if (message.enabled) {
          start();
        } else {
          stop();
        }
      }
    );

  chrome.runtime.sendMessage(
    {
      type:
        "notf11-video-float-state"
    },
    (response) => {
      if (
        chrome.runtime.lastError
      ) {
        start();
        return;
      }

      if (
        response?.enabled === false
      ) {
        stop();
      } else {
        start();
      }
    }
  );
})();
