# X/Y 축 커플링 보정(디커플링) 수식 정리

레이저(또는 미러/서보) 조준에서 **PAN(x) PWM**을 바꾸면 화면상의 \(X\)뿐 아니라 \(Y\)도 같이 변하고, **TILT(y) PWM**을 바꿔도 \(X\)가 같이 변하는 현상은 전형적인 **크로스 커플링(cross-coupling)** 입니다.  
이를 수학적으로 다루는 가장 실용적인 방법은 “현재 조준 근처에서의 국소 선형 모델(야코비안)”로 잡고, 그 역행렬로 **디커플링 제어**를 하는 것입니다.

---

## 1) 변수 정의

- **입력(제어량)**: PWM 펄스폭(µs)
  - \(p\): PAN(=GPIO12, pwm0)
  - \(t\): TILT(=GPIO13, pwm1)
  - \(\Delta u = \begin{bmatrix}\Delta p\\ \Delta t\end{bmatrix}\)

- **출력(관측량)**: 카메라/화면 좌표(픽셀)
  - \(X, Y\): 타겟(레이저 스팟) 화면 좌표
  - \(\Delta s = \begin{bmatrix}\Delta X\\ \Delta Y\end{bmatrix}\)

- **오차 벡터**
  - 목표 좌표 \((X^\*, Y^\*)\), 현재 좌표 \((X, Y)\)
  - \(e = \begin{bmatrix}e_X\\ e_Y\end{bmatrix} = \begin{bmatrix}X - X^\*\\ Y - Y^\*\end{bmatrix}\)
  - (부호는 구현에 따라 “목표-현재”로 써도 됨. 아래 제어식의 부호만 일관되면 됨.)

---

## 2) 국소 선형 모델 (야코비안 \(J\))

현재 작업점(현재 PWM \((p_0,t_0)\), 현재 좌표 \((X_0,Y_0)\)) 근처에서

\[
\Delta s \approx J\,\Delta u
\]

\[
J=
\begin{bmatrix}
\frac{\partial X}{\partial p} & \frac{\partial X}{\partial t}\\
\frac{\partial Y}{\partial p} & \frac{\partial Y}{\partial t}
\end{bmatrix}
\]

여기서 **비대각 원소** \(\frac{\partial X}{\partial t}\), \(\frac{\partial Y}{\partial p}\) 가 “축이 섞이는 정도”입니다.

---

## 3) \(J\) 추정(실측) — 4번 측정으로 끝

작은 PWM 변화량 \(\delta\) (예: 10~30µs, *반응이 확실히 보이는 값*)을 정합니다.

1. 기준점 측정  
   \((p_0, t_0)\to (X_0, Y_0)\)

2. PAN만 증가  
   \((p_0+\delta, t_0)\to (X_1, Y_1)\)

3. TILT만 증가  
   \((p_0, t_0+\delta)\to (X_2, Y_2)\)

유한차분으로 편미분을 근사합니다.

\[
\frac{\partial X}{\partial p}\approx \frac{X_1-X_0}{\delta},\quad
\frac{\partial Y}{\partial p}\approx \frac{Y_1-Y_0}{\delta}
\]

\[
\frac{\partial X}{\partial t}\approx \frac{X_2-X_0}{\delta},\quad
\frac{\partial Y}{\partial t}\approx \frac{Y_2-Y_0}{\delta}
\]

즉,

\[
J \approx
\begin{bmatrix}
\frac{X_1-X_0}{\delta} & \frac{X_2-X_0}{\delta}\\
\frac{Y_1-Y_0}{\delta} & \frac{Y_2-Y_0}{\delta}
\end{bmatrix}
\]

### 실무 팁
- **잡음**이 크면 \(\delta\)를 키우거나, 위 과정을 여러 번 반복해 평균을 내면 안정적입니다.
- \(\delta\)는 너무 작으면(예: 1~2µs) 물리 데드밴드 때문에 \((X,Y)\)가 거의 안 변해 추정이 망가질 수 있습니다.

---

## 4) 디커플링 제어식 (핵심)

원하는 방향(오차 감소)으로 화면 좌표를 움직이려면

\[
\Delta u = -\alpha\,J^{-1}e
\]

- \(\alpha\in(0,1]\): 업데이트 이득(작을수록 안정, 클수록 빠름)
- 적용 후 PWM 갱신:
  - \(p \leftarrow p + \Delta p\)
  - \(t \leftarrow t + \Delta t\)
  - 이후 PWM 범위 \([p_{\min},p_{\max}]\), \([t_{\min},t_{\max}]\)로 클램프

### \(2\times2\) 역행렬을 직접 쓰는 형태

\[
J=
\begin{bmatrix}
a & b\\
c & d
\end{bmatrix},
\quad
\det(J)=ad-bc
\]

\[
J^{-1}=
\frac{1}{ad-bc}
\begin{bmatrix}
d & -b\\
-c & a
\end{bmatrix}
\]

따라서

\[
\begin{bmatrix}\Delta p\\ \Delta t\end{bmatrix}
=
-\alpha\frac{1}{ad-bc}
\begin{bmatrix}
d & -b\\
-c & a
\end{bmatrix}
\begin{bmatrix}e_X\\ e_Y\end{bmatrix}
\]

---

## 5) 주의사항 (중요)

### 5.1) 특이/불안정(역행렬이 폭주)
- \(|\det(J)|\)가 너무 작으면 \(J^{-1}\)이 커져서 작은 오차에도 \(\Delta p,\Delta t\)가 과도해질 수 있습니다.
- 실무에서는 다음 중 하나를 권장합니다.
  - \(|\det(J)|<\epsilon\)이면 디커플링을 끄고(또는) \(\alpha\)를 크게 줄이기
  - \(\Delta p,\Delta t\)에 최대 변화량 제한(예: 한 스텝당 ±20µs)

### 5.2) 작업점 의존(선형 근사는 “그 근처”에서만 정확)
- \(J\)는 화면의 위치/각도에 따라 달라질 수 있습니다.
- 정밀을 원하면:
  - 화면을 그리드로 나눠 **셀마다 \(J\)** 를 추정해 저장(LUT)
  - 또는 주기적으로 \(J\)를 재추정(online 업데이트)

### 5.3) 좌표계/부호 일관성
- \(e\)를 “현재-목표”로 정의했는지 “목표-현재”로 정의했는지에 따라 제어식의 부호가 달라집니다.
- 한 번 정하면 끝까지 통일하세요.

---

## 6) 최소 적용 절차(요약)

1. 현재 PWM \((p_0,t_0)\)에서 \((X_0,Y_0)\) 측정  
2. \(\delta\)만큼 \(p\) 변화 → \((X_1,Y_1)\) 측정  
3. \(\delta\)만큼 \(t\) 변화 → \((X_2,Y_2)\) 측정  
4. 위 식으로 \(J\) 계산  
5. 매 제어 주기마다 \(\Delta u=-\alpha J^{-1}e\) 로 PWM 업데이트

