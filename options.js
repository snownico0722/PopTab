(() => {
  const STORAGE_KEY = "closeBehavior";
  const DEFAULT_BEHAVIOR = "discard";
  const VALID_BEHAVIORS = new Set([
    "discard",
    "writeback"
  ]);

  const inputs = Array.from(
    document.querySelectorAll(
      'input[name="closeBehavior"]'
    )
  );
  const status = document.getElementById("status");

  function applyValue(value) {
    const selected =
      VALID_BEHAVIORS.has(value)
        ? value
        : DEFAULT_BEHAVIOR;

    for (const input of inputs) {
      input.checked =
        input.value === selected;
    }
  }

  async function load() {
    const result =
      await chrome.storage.local.get(
        STORAGE_KEY
      );

    applyValue(
      result[STORAGE_KEY]
    );
  }

  async function save(value) {
    if (!VALID_BEHAVIORS.has(value)) {
      return;
    }

    await chrome.storage.local.set({
      [STORAGE_KEY]: value
    });

    status.textContent = "已保存";
    window.setTimeout(() => {
      status.textContent = "";
    }, 1200);
  }

  for (const input of inputs) {
    input.addEventListener(
      "change",
      () => {
        if (input.checked) {
          save(input.value).catch(
            (error) => {
              console.error(
                "PopTab settings save failed:",
                error
              );
              status.textContent =
                "保存失败";
            }
          );
        }
      }
    );
  }

  chrome.storage.onChanged.addListener(
    (changes, areaName) => {
      if (
        areaName !== "local" ||
        !changes[STORAGE_KEY]
      ) {
        return;
      }

      applyValue(
        changes[STORAGE_KEY]
          .newValue
      );
    }
  );

  load().catch((error) => {
    console.error(
      "PopTab settings load failed:",
      error
    );
    status.textContent =
      "读取设置失败";
  });
})();
