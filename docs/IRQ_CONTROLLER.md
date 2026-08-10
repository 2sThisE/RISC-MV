# Programmable IRQ Controller

IRQ Controller는 외부 장치 IRQ 0~47의 mask, pending, active 상태와 대상 논리 프로세서를 관리한다. 물리 MMIO 범위는 `0xFFFFFFFFFFFC3000`~`0xFFFFFFFFFFFC30FF`다. 모든 레지스터는 64비트이며 8바이트 정렬 `LOAD64`/`STORE64`로 접근한다.

IRQ 48~63은 IPI 전용이며 이 컨트롤러를 우회해 Core Control이 선택한 하드웨어 스레드로 직접 전달한다.

## 레지스터

| 오프셋 | 이름 | 접근 | 의미 |
|---:|---|---|---|
| `0x00` | `MAGIC` | R | `0x43515249` (`IRQC`) |
| `0x08` | `VERSION` | R | 현재 1 |
| `0x10` | `LINE_COUNT` | R | 외부 IRQ 수, 현재 48 |
| `0x18` | `TARGET_COUNT` | R | 논리 프로세서 수 |
| `0x20` | `ENABLE` | R/W | 활성 IRQ bitmap 전체 읽기/교체 |
| `0x28` | `ENABLE_SET` | W | 1인 IRQ를 활성화 |
| `0x30` | `ENABLE_CLEAR` | W | 1인 IRQ를 mask |
| `0x38` | `PENDING` | R | 아직 target에 전달되지 않은 IRQ |
| `0x40` | `ACTIVE` | R | 전달되어 EOI를 기다리는 IRQ |
| `0x48` | `LINE_SELECT` | R/W | route를 읽거나 설정할 IRQ 번호 |
| `0x50` | `ROUTE` | R/W | 선택한 IRQ의 논리 프로세서 번호 |
| `0x58` | `EOI` | W | 처리를 끝낸 IRQ 번호 |
| `0x60` | `RESULT` | R | 마지막 설정 결과 |
| `0x68` | `FEATURES` | R | masking, routing, EOI, fixed priority, no-reentry |

`RESULT`는 `0=NONE`, `1=SUCCESS`, `2=INVALID_LINE`, `3=INVALID_TARGET`, `4=BUSY`, `5=INVALID_MASK`, `6=NOT_ACTIVE`다. 명령형 레지스터에 잘못된 값을 써도 STORE 자체는 성공하고 오류는 `RESULT`에 기록된다. active IRQ의 route 변경은 `BUSY`다.

## 전달 규칙

reset 직후 모든 외부 IRQ는 masked 상태이고 모든 route는 논리 프로세서 0이다. 장치가 masked IRQ를 올리면 `PENDING`에 보존된다. 해당 IRQ를 활성화하면 지정 target이 비어 있을 때 전달되고 `ACTIVE`로 이동한다.

한 논리 프로세서에는 active 외부 IRQ 하나만 둔다. 같은 target의 다른 IRQ는 EOI까지 pending에 남으며, 여러 IRQ가 기다리면 번호가 가장 낮은 IRQ부터 전달한다. 이미 active인 같은 IRQ가 다시 발생하면 한 pending 비트로 합쳐지고 EOI 뒤 한 번 재전달되므로 재진입과 이벤트 손실을 함께 막는다.

IPI는 외부 IRQ보다 높은 별도 우선순위 class다. IPI가 여러 개면 48부터 낮은 번호 순서로 선택하고, IPI가 없을 때 외부 IRQ를 선택한다.

핸들러는 장치 쪽 원인을 먼저 제거한 뒤 controller EOI를 기록해야 한다.

```text
장치 상태 읽기
→ 데이터 또는 이벤트 처리
→ 장치 IRQ_STATUS ACK
→ STORE64 [IRQ_CONTROLLER_EOI], irq_line
→ IRET
```

EOI를 생략하면 해당 IRQ는 active로 남고 같은 target의 다음 외부 IRQ도 전달되지 않는다. IPI는 별도 경로이므로 외부 IRQ가 active여도 pending 될 수 있지만 CPU의 `INTERRUPT_ENABLE` 규칙은 동일하게 적용된다.

## 초기 설정 예

```text
STORE64 [LINE_SELECT], irq_line
STORE64 [ROUTE],       logical_processor
STORE64 [ENABLE_SET],  1 << irq_line
EI
```

VBR과 IRQ 벡터, 커널 스택은 IRQ를 활성화하기 전에 먼저 설정해야 한다.
