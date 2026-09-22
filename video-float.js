(() => {
  const MIN_WIDTH = 240;
  const MIN_HEIGHT = 135;
  const BUTTON_GAP = 8;
  const HOVER_LEFT = 150;
  const HOVER_RIGHT = 24;
  const HOVER_ABOVE = 58;
  const HOVER_BELOW = 56;

  let activeVideo = null;
  let button = null;
  let resizeObserver = null;
  let scanScheduled = false;
  let lastPointer = null;

  function isVisibleVideo(video) {
    const rect =
      video.getBoundingClientRect();

    if (
      rect.width < MIN_WIDTH ||
      rect.height < MIN_HEIGHT
    ) {
      return false;
    }

    if (
      rect.right <= 0 ||
      rect.bottom <= 0 ||
      rect.left >= window.innerWidth ||
      rect.top >= window.innerHeight
    ) {
      return false;
    }

    const style =
      window.getComputedStyle(video);

    return (
      style.display !== "none" &&
      style.visibility !== "hidden" &&
      Number.parseFloat(style.opacity || "1") > 0
    );
  }

  function visibleArea(video) {
    const rect =
      video.getBoundingClientRect();

    const width =
      Math.max(
        0,
        Math.min(rect.right, window.innerWidth) -
          Math.max(rect.left, 0)
      );

    const height =
      Math.max(
        0,
        Math.min(rect.bottom, window.innerHeight) -
          Math.max(rect.top, 0)
      );

    return width * height;
  }

  function findPrimaryVideo() {
    let best = null;
    let bestArea = 0;

    for (
      const video of
      document.querySelectorAll("video")
    ) {
      if (!isVisibleVideo(video)) {
        continue;
      }

      const area =
        visibleArea(video);

      if (area > bestArea) {
        best = video;
        bestArea = area;
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
        border: "1px solid rgba(255,255,255,.18)",
        borderRadius: "7px",
        background: "rgba(24,24,27,.88)",
        color: "#fff",
        font: "12px/1.2 -apple-system,BlinkMacSystemFont,'Segoe UI','Microsoft YaHei',sans-serif",
        cursor: "pointer",
        boxShadow: "0 2px 10px rgba(0,0,0,.25)",
        backdropFilter: "blur(8px)",
        WebkitBackdropFilter: "blur(8px)",
        opacity: "0",
        transform: "translateY(3px)",
        transition: "opacity 120ms ease, transform 120ms ease",
        pointerEvents: "none",
        userSelect: "none"
      }
    );

    button.addEventListener(
      "mouseenter",
      () => {
        showButton();
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
            button.disabled = false;

            if (
              chrome.runtime.lastError
            ) {
              console.debug(
                "NotF11 video float:",
                chrome.runtime.lastError.message
              );
            }
          }
        );
      },
      true
    );

    document.documentElement.appendChild(
      button
    );

    return button;
  }

  function hideButton() {
    if (!button) {
      return;
    }

    button.style.opacity = "0";
    button.style.transform =
      "translateY(3px)";
    button.style.pointerEvents =
      "none";
  }

  function showButton() {
    if (
      !activeVideo ||
      document.fullscreenElement
    ) {
      hideButton();
      return;
    }

    positionButton();
    button.style.opacity = "1";
    button.style.transform =
      "translateY(0)";
    button.style.pointerEvents =
      "auto";
  }

  function positionButton() {
    if (
      !activeVideo ||
      !button
    ) {
      return;
    }

    const rect =
      activeVideo.getBoundingClientRect();

    const buttonRect =
      button.getBoundingClientRect();

    const left =
      Math.min(
        window.innerWidth -
          buttonRect.width -
          8,
        Math.max(
          8,
          rect.right -
            buttonRect.width
        )
      );

    const top =
      Math.max(
        8,
        rect.top -
          buttonRect.height -
          BUTTON_GAP
      );

    button.style.left =
      `${Math.round(left)}px`;

    button.style.top =
      `${Math.round(top)}px`;
  }

  function pointerNearVideoCorner(
    x,
    y
  ) {
    if (!activeVideo) {
      return false;
    }

    const rect =
      activeVideo.getBoundingClientRect();

    return (
      x >= rect.right - HOVER_LEFT &&
      x <= rect.right + HOVER_RIGHT &&
      y >= rect.top - HOVER_ABOVE &&
      y <= rect.top + HOVER_BELOW
    );
  }

  function pointerOverButton(
    x,
    y
  ) {
    if (
      !button ||
      button.style.pointerEvents ===
        "none"
    ) {
      return false;
    }

    const rect =
      button.getBoundingClientRect();

    return (
      x >= rect.left &&
      x <= rect.right &&
      y >= rect.top &&
      y <= rect.bottom
    );
  }

  function refreshHoverState() {
    if (
      !lastPointer ||
      !activeVideo
    ) {
      hideButton();
      return;
    }

    if (
      pointerNearVideoCorner(
        lastPointer.x,
        lastPointer.y
      ) ||
      pointerOverButton(
        lastPointer.x,
        lastPointer.y
      )
    ) {
      showButton();
    } else {
      hideButton();
    }
  }

  function observeActiveVideo(video) {
    if (resizeObserver) {
      resizeObserver.disconnect();
      resizeObserver = null;
    }

    activeVideo = video;

    if (!video) {
      hideButton();
      return;
    }

    resizeObserver =
      new ResizeObserver(() => {
        positionButton();
        refreshHoverState();
      });

    resizeObserver.observe(video);

    ensureButton();
    positionButton();
    refreshHoverState();
  }

  function scan() {
    scanScheduled = false;

    const nextVideo =
      findPrimaryVideo();

    if (
      nextVideo !== activeVideo
    ) {
      observeActiveVideo(
        nextVideo
      );
      return;
    }

    positionButton();
    refreshHoverState();
  }

  function scheduleScan() {
    if (scanScheduled) {
      return;
    }

    scanScheduled = true;

    requestAnimationFrame(scan);
  }

  document.addEventListener(
    "pointermove",
    (event) => {
      lastPointer = {
        x: event.clientX,
        y: event.clientY
      };

      refreshHoverState();
    },
    {
      capture: true,
      passive: true
    }
  );

  document.addEventListener(
    "pointerleave",
    () => {
      lastPointer = null;
      hideButton();
    },
    {
      capture: true,
      passive: true
    }
  );

  window.addEventListener(
    "scroll",
    scheduleScan,
    {
      capture: true,
      passive: true
    }
  );

  window.addEventListener(
    "resize",
    scheduleScan,
    {
      passive: true
    }
  );

  document.addEventListener(
    "fullscreenchange",
    scheduleScan
  );

  const mutationObserver =
    new MutationObserver(
      (records) => {
        const externalChange =
          records.some(
            (record) =>
              record.target !==
                button &&
              !button?.contains(
                record.target
              )
          );

        if (externalChange) {
          scheduleScan();
        }
      }
    );

  mutationObserver.observe(
    document.documentElement,
    {
      childList: true,
      subtree: true,
      attributes: true,
      attributeFilter: [
        "class",
        "style",
        "hidden"
      ]
    }
  );

  scheduleScan();
})();
