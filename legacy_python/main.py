import cv2
from pathlib import Path
from ultralytics import YOLO

import re
from pathlib import Path

import easyocr

reader = easyocr.Reader(["en"], gpu=False)

ALLOWED_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


# Модель должна быть обучена на class: license_plate
MODEL_PATH = Path(__file__).resolve().with_name("license_plate_detector.pt")
model = YOLO(str(MODEL_PATH))

def normalize_plate(text):
    text = re.sub(r"[^A-Z0-9]", "", text.upper())

    # Для этого формата ожидаем:
    # 2 буквы + 2 цифры + 2 буквы
    if len(text) != 6:
        return text

    chars = list(text)

    # Позиции 0,1,4,5 должны быть буквами
    letter_map = {
        "0": "O",
        "1": "I",
        "2": "Z",
        "5": "S",
        "8": "B",
    }

    # Позиции 2,3 должны быть цифрами
    digit_map = {
        "O": "0",
        "Q": "0",
        "I": "1",
        "L": "1",
        "Z": "2",
        "S": "5",
        "B": "8",
        "G": "6",
    }

    for i in [0, 1, 4, 5]:
        if chars[i] in letter_map:
            chars[i] = letter_map[chars[i]]

    for i in [2, 3]:
        if chars[i] in digit_map:
            chars[i] = digit_map[chars[i]]

    return "".join(chars)



def recognize_plate(plate_image):
    if plate_image is None or plate_image.size == 0:
        return None, 0.0

    # Увеличиваем изображение
    plate = cv2.resize(
        plate_image,
        None,
        fx=3,
        fy=3,
        interpolation=cv2.INTER_CUBIC
    )

    h, w = plate.shape[:2]

    # В этом типе номера основной номер находится снизу.
    # Убираем верхнюю часть с 日本 337.
    plate = plate[int(h * 0.38):h, :]

    gray = cv2.cvtColor(
        plate,
        cv2.COLOR_BGR2GRAY
    )

    # Улучшаем контраст
    gray = cv2.equalizeHist(gray)

    # Небольшое сглаживание
    gray = cv2.GaussianBlur(
        gray,
        (3, 3),
        0
    )

    results = reader.readtext(
        gray,
        allowlist=ALLOWED_CHARS,
        detail=1,
        paragraph=False
    )

    if not results:
        return None, 0.0

    candidates = []

    for bbox, text, confidence in results:
        text = re.sub(
            r"[^A-Z0-9]",
            "",
            text.upper()
        )

        if len(text) < 4:
            continue

        x1 = bbox[0][0]
        y1 = bbox[0][1]

        x2 = bbox[2][0]
        y2 = bbox[2][1]

        width = x2 - x1
        height = y2 - y1

        area = width * height

        candidates.append({
            "text": text,
            "confidence": float(confidence),
            "area": area
        })

    if not candidates:
        return None, 0.0

    # Берём самый крупный распознанный текст
    candidates.sort(
        key=lambda item: (
            item["area"],
            item["confidence"]
        ),
        reverse=True
    )

    best = candidates[0]

    normalized = normalize_plate(best["text"])

    return normalized, best["confidence"]


def detect_license_plate(frame):
    results = model(frame, imgsz=640, conf=0.5, verbose=False)[0]
    plates = []

    for box in results.boxes:
        x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
        confidence = float(box.conf[0])
        plates.append((x1, y1, x2, y2, confidence))

    return plates


def main():
    camera = cv2.VideoCapture("/Users/yerek/rbt/parking/video/car.mp4")

    if not camera.isOpened():
        raise RuntimeError("Cannot open video")

    last_number = None
    frame_number = 0

    while True:
        ret, frame = camera.read()
        if not ret:
            print("Failed to read frame")
            break

        frame_number += 1

        plates = detect_license_plate(frame)
        for x1, y1, x2, y2, confidence in plates:
            plate_crop = frame[y1:y2, x1:x2]

            if frame_number % 5 == 0:
                number, ocr_confidence = recognize_plate(plate_crop)
                if number:
                    if number != last_number:
                        print(
                            f"🚗 Номер: {number} "
                            f"| detector: {confidence:.2f} "
                            f"| OCR: {ocr_confidence:.2f}"
                        )
                    last_number = number


            cv2.rectangle(frame, (x1, y1), (x2, y2), (0, 255, 0), 2)

            if last_number:
                cv2.putText(
                    frame,
                    last_number,
                    (x1, max(30, y1 - 10)),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.8,
                    (0, 255, 0),
                    2
                )

            cv2.putText(
                frame,
                f"Plate {confidence:.2f}",
                (x1, y1 - 10),
                cv2.FONT_HERSHEY_SIMPLEX,
                0.7,
                (0, 255, 0),
                2
            )

            cv2.imshow("Номер машины", plate_crop)
        

        cv2.imshow("Camera", frame)
       

        if cv2.waitKey(1) & 0xFF == ord('q'):
            break

    camera.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
