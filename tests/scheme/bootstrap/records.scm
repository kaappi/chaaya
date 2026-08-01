;; R7RS define-record-type

(define-record-type <pare>
  (kons x y)
  pare?
  (x kar set-kar!)
  (y kdr))

(check-assert "pare?" (pare? (kons 1 2)))
(check-assert "not pair" (not (pare? (cons 1 2))))
(check-eqv "kar" 1 (kar (kons 1 2)))
(check-eqv "kdr" 2 (kdr (kons 1 2)))
(check-eqv "set-kar!" 3
  (let ((k (kons 1 2)))
    (set-kar! k 3)
    (kar k)))

(define-record-type point
  (make-point x y)
  point?
  (x point-x)
  (y point-y set-point-y!))

(check-eqv "point-x" 10 (point-x (make-point 10 20)))
(check-eqv "set-point-y!" 99
  (let ((p (make-point 1 2)))
    (set-point-y! p 99)
    (point-y p)))
(check-assert "distinct types" (not (point? (kons 1 2))))

(check-finish)
