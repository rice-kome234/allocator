> 過去版の保存です。現行ソリューションのビルド対象外で、以下は変更前の仕様です。

# ポインタをまとめて走査する Slab Pool（C++20）

実体は通常の new/delete や既存アロケータで生成・破棄し、SlabPool は **実体を指すポインタだけ**を密に保持します。既存のコンポーネントが持つ next を一個ずつ辿る代わりに、連続したポインタ列から実体へアクセスするための独立検証プロジェクトです。

実体のメモリ配置は変わりません。Slab 内のポインタは連続しますが、別 Slab との隣接や参照先のキャッシュヒットは保証しません。列挙と Update の時間に加え、ポインタの追加・容量拡張を別の測定で比較します。実体の生成・破棄は計測に含めません。

## 現在の中心機能: ID別の一括コピーと範囲for

実体のコピーではなく、生成済みインスタンスの **ポインタ値だけ**をコピーします。mainのEnemy/Bulletはstaticな数値Idを持ち、SlabPoolはIDごとに別のポインタ列を保持します。実体の所有権・生成・破棄は利用側に残ります。

~~~cpp
// Enemy::Id は利用側が決めた数値ID。pointersは外部実体を指すvoid*の配列/span。
slab::SlabPool pool{};
if (!pool.Register(Enemy::Id, {0, 64})) { return false; }
if (pool.AddBatch(Enemy::Id, pointers) == slab::SlabPool::InvalidIndex) { return false; }

for (auto& enemy : pool.GetRange<Enemy>(Enemy::Id)) {
    enemy.Update(1.0f / 60.0f);
}
~~~

AddBatchは入力を検査してから不足容量を一括確保し、ポインタ列をコピーします。初期容量0で全件を渡すと、一つのSlabへまとめられます。後から増える場合は新しいSlabを追加し、既存のポインタ格納領域と実体は移動しません。Slab同士の隣接は保証しません。GetRangeはSlab境界を越えて全件を返す前方範囲で、内部ではSlab境界だけで次の連続範囲を取得します。GetBatchは従来どおり一つのSlab内だけを返します。

コンテナにはvectorを使いません。固定Slabは通常のアラインメント付きnewで確保します。仮想アドレス予約・段階コミットは今回の実装には含めていません。Slab索引の管理ポインタ配列は準備時に拡張・コピーされる場合がありますが、登録済みの全ポインタ列を再配置する拡張ではありません。

### 並列登録と並列更新

mainからは測定・使用例用の補助クラスへ処理を渡せます。呼び出しが戻ると全ワーカーが完了しています。

~~~cpp
benchmarks::ParallelTasks tasks{14};
if (!tasks.CopyPointers(pool, Enemy::Id, pointers, 256)) { return false; }
tasks.ForEach(pool, Enemy::Id, &Enemy::Update, 1.0f / 60.0f);
~~~

これはAddBatchの代わりに使う例です。同じ入力をAddBatchとCopyPointersで二重登録しないでください。型や実体をスレッドへ固定せず、毎回の登録数を分担します。並列処理のクラスはbenchmarksに置き、SlabPool本体にはスレッドの作成処理を入れていません。

既存の実行環境に組み込む場合は次の3段階を使います。

1. 全ワーカー停止中に `BeginConcurrentAppend(id, maximumAdditionalCount)` で容量を確保する。全IDの構成・索引を確定してからワーカーを開始する。
2. 各ワーカーから `AddBatchConcurrent(id, span)` を呼ぶ。CASでバッチの区間を予約し、ポインタ値だけをコピーする。準備した上限を超えた入力やnull混入は、区間を消費せず失敗する。バッチ内の入力順は保持し、バッチ間の登録順は不定。
3. **全ワーカーをjoin/barrier等で同期してから** `EndConcurrentAppend(id)` を呼び、その後に走査する。End自身は完了待ちをしない。

CASは区間の重複を防ぐ目的だけに使用し、成功・失敗ともmemory_order_relaxedです。予約した時点ではコピーが未完了の可能性があるため、予約末尾と公開件数は分離しています。BeginからEndの間、対象IDのCount/GetBatch/GetRangeは0または空を返しますが、並列実行中に許可する呼び出しはAddBatchConcurrent同士だけです。通常Add・削除・Clear・Reserve・統計取得・ID登録・破棄・Endを同時に実行してはいけません。

`atomic<size_t>::is_always_lock_free`をコンパイル時に確認しています。並列一括登録の区間予約はロックフリーですが、容量確保・OS・スレッド起動・完了待ちを含む処理全体がロックフリーという意味ではありません。CASを使わない単独スレッドのAddBatchも残しています。従来のAddConcurrentは自動拡張を含むmutex方式で、比較用・従来の呼び出し元用です。新しい並列登録フェーズと混在させないでください。

Range/Batchの利用中は登録列を変更せず、実体も生存させます。実体の重複登録や共有データへの同時Updateは利用側で防ぎます。削除は既存のRemoveAt/Clearを同期後に使用します。削除キューや実体の所有機構は追加していません。

### 既定の測定

mainをそのまま実行すると、14項目のテスト、一括登録と並列更新の使用例、新しい一括登録ベンチマークを実行して終了します。旧vector比較と対話テストはmainのdefineで必要時だけ有効にします。

`benchmarks/bulk_benchmark.cpp`は1万/10万件を同じ容量の1 Slabに確保し、主スレッドの一括コピー、mutexで1個ずつ、CASで1個ずつ、CASで256個ずつを比較します。走査は全方式で1/10/14ワーカー。全方式を1回慣らした後、方式順を交代して5回測定し、一括確保・登録・走査を分けて表示します。走査は各走行16回の平均です。実体とスレッドの生成/破棄・結果検証は測定外で、並列呼び出しの通知/完了待ち・補助処理は測定内です。

ポインタは同じ外部実体を参照しますが、並列登録の順序は不定です。方式間の走査時間差には並び順やキャッシュ状態も影響します。キャッシュミス率やゲーム全体のフレーム時間は計測していません。5走行の観測最大値は時間上限やp99ではありません。

測定結果: [一括登録ログ](docs/benchmark-pointer-bulk-quick.txt)、[検証記録](docs/validation.md)。

## ファイル

- include/slab/slab_pool.h: 公開インターフェースと Doxygen。
- include/slab/slab_pool_impl.h: 型付きBatchと全Slabを走査するRangeのテンプレート実装。
- include/slab/slab_pool.cpp: Slab確保、一括コピー、CAS予約、末尾交換、範囲取得の実装。
- main.cpp: クラス定義、new/delete、登録・解除、走査と Update 関数の指定。
- benchmarks/growth_benchmark.cpp: reserve有無の追加全体と、拡張を伴う追加の時間・再配置対象数の比較。
- benchmarks/: 時間比較、parallel_executor の並列更新、parallel_tasks の任意本数での並列作業、interactive_test の入力・表示を担当。
- tests/: 容量・削除・寿命・並列更新・1/10/14スレッド同時追加の自動テスト。

## 基本の使い方

以下は、既存の Enemy 型を使う場合の例です。

~~~cpp
slab::SlabPool pool{};
constexpr slab::SlabPool::PoolId enemyId{1};

slab::SlabPool::Options options{};
options.initialCapacity = 100; // ポインタ100個分を一つのSlabとして準備。
options.growthBytes = 64;     // 追加は64バイト。x64ではポインタ8個分。

if (!pool.Register(enemyId, options)) {
    return false;
}

Enemy* enemy{new (std::nothrow) Enemy{100}};
if (enemy == nullptr) {
    return false;
}

const auto index{pool.Add(enemyId, enemy)};
if (index == slab::SlabPool::InvalidIndex) {
    delete enemy;
    return false;
}

// 同じSlab内の連続したポインタ列を、一度に取得する。
std::size_t first{};
const auto count{pool.Count(enemyId)};
while (first < count) {
    const auto batch{pool.GetBatch<Enemy>(enemyId, first)};
    for (auto& enemy : batch) {
        enemy.Update(1.0f / 60.0f);
    }
    first += batch.size();
}

// 登録を外してから、実体を元の確保方法に対応する処理で破棄する。
const auto result{pool.RemoveAt(enemyId, index)};
delete static_cast<Enemy*>(result.removed);
~~~

Add は外部で生成したポインタを受け取ります。sizeof(T)・alignof(T) の登録、placement new、construct_at は不要です。型や数値 ID の対応は利用側で決めます。

### 型付きの走査

数値 ID は実行時の値なので、型は GetBatch<Enemy> のように明示します。戻り値はポインタ配列を複製しないビューです。

~~~cpp
for (auto& enemy : pool.GetBatch<Enemy>(enemyId, 0)) {
    enemy.Update(1.0f / 60.0f); // enemy は Enemy&。
}

for (auto& enemy : pool.GetBatch<const Enemy>(enemyId, 0)) {
    // enemy は const Enemy&。メンバの読み取りだけを行う。
    std::cout << enemy.health << '\n';
}
~~~

実体を参照するので、メンバへのアクセスは . を使います。Batch 自体が const でも、T が非constなら実体は変更可能です。読み取り専用にする場合は T に const を付けてください。

型付き版も一つの Slab の範囲を返します。全体の走査では基本例のように first を batch.size() ずつ進めます。登録時のポインタを T* に戻せることは利用側で保証してください。数値 ID と型の一致を実行時に検査する仕組みは追加していません。

内部では void* を一つずつ T* に変換して参照を返し、void** を T** として読み替えません。実体やポインタ配列の追加確保・コピーは行いません。イテレータを使う間は Batch と Slab の両方を有効に保ち、Pool の変更は走査後に行ってください。


## API と所有権

| API | 役割 |
|---|---|
| Register(id, options) | ID と初期ポインタ個数・追加バイト数を登録。同一設定の再登録は成功 |
| Reserve(id, capacity) | 総容量を予約。不足分を一つの新しい Slab に確保（少なくとも追加容量分） |
| AddBatch(id, span) | ポインタ列の一括コピー。不足容量をまとめて確保 |
| BeginConcurrentAppend / AddBatchConcurrent / EndConcurrentAppend | 容量準備、CASで区間予約してコピー、完了同期後の公開 |
| GetRange<T>(id, first, maximumCount) | Slabをまたいで全件または指定範囲をT&で列挙 |
| Add(id, pointer) | 非所有ポインタを末尾へ追加。ロックなし |
| AddConcurrent(id, pointer) | 同時追加用。IDごとのmutexで追加・拡張を保護して添字を返す |
| RemoveAt(id, index) | 末尾と入れ替えて削除。削除した実体と、削除位置へ移った実体を返す |
| Clear(id) | 登録数を0にする。実体は破棄せず、Slabも保持 |
| Count / At | 登録数・指定添字のポインタを取得 |
| GetBatch(id, first, maximumCount) | 同じSlab内の登録済みポインタを読み取り専用spanで返す |
| GetBatch<T>(id, first, maximumCount) | 同じ範囲の実体をT&で列挙する。const Tなら読み取り専用 |
| GetStatistics() | ポインタ領域と管理領域の要求量を集計。実体のメモリは含まない |

- 実体の所有権は常に利用側にあります。RemoveAt、Clear、Pool のデストラクタは実体を delete しません。
- 破棄前に登録を解除してください。Clear で一括解除する場合も、実体を破棄できる情報は利用側で保持してください。
- nullptr は登録できません。同じ実体の重複登録は利用側で防いでください。Add では重複検索を行いません。
- RemoveAt は更新順序を維持しません。戻り値 moved がある場合、そのポインタが削除位置へ移っています。添字を実体に記録している場合は更新が必要です。
- 添字は固定ハンドルではありません。世代番号や古い添字の検出はありません。
- 実体は移動しません。容量拡張でコピーするのは Slab 索引の管理ポインタだけです。
- initialCapacity は最初の Slab に確保するポインタ個数で、既定64個、0は遅延確保です。100個ならx64で800バイトを一つのSlabに確保します。
- growthBytes は不足時に追加するポインタ領域のバイト数です。既定64バイト（x64で8個分）で、sizeof(void*) の正の倍数を指定できます。旧 growthCapacity（個数）は growthBytes（バイト数）へ変更しました。
- 初期・追加・Reserve のどの Slab も、ポインタ領域の先頭を64バイト境界に揃えます。SlabAlignment は固定64で、実行CPUから自動取得する設定ではありません。
- 初期個数と Reserve の不足個数は64バイト単位へ丸めません。Reserve は不足個数と growthBytes / sizeof(void*) の大きい方を、一つの新しいSlabとして確保します。
- 64バイトはポインタ領域だけの要求サイズです。Slabヘッダー・索引・アロケータの管理領域やアラインメントの余白は別に必要です。
- 索引の二分探索は Slab 範囲の取得時に行います。GetBatch の内側は連続したポインタ列を走査します。
- ポインタ配列、Slab ヘッダー、ID 管理情報、索引は別の管理領域です。実体の確保は Pool 内では行いません。
- C++ ソースに try/catch/throw は使っていません。Pool の確保は nothrow new、失敗は false / InvalidIndex です。ポインタ領域はアラインメント付き operator new で確保し、対応する operator delete で解放します。
- 使用例・テスト・測定の標準コンテナやスレッドの資源不足は回復処理の対象外です。

## 並列更新と対話テスト

対話テストでは変更操作を主スレッドで行い、完了後にワーカーが走査します。現在の使用例はCopyPointersによる並列一括登録を使い、従来の追加比較ではAddConcurrentによる同時追加を行います。いずれも、追加中と走査中のフェーズを分けます。

main.cpp はコンポーネントの生成・登録・走査・解除・破棄を定義します。並列処理は benchmarks/parallel_executor に任せ、メンバ関数を次のように渡します。

~~~cpp
benchmarks::ParallelExecutor executor{};

executor.ForEach(pool, enemyId, &Enemy::Update, deltaSeconds);
executor.ForEach(pool, bulletId, &Bullet::Update, deltaSeconds);
~~~

ForEach は同期呼び出しで、戻った時点でその ID の全対象が更新済みです。この例では Enemy 全体の更新完了後に Bullet 全体を更新します。4 本のワーカーは実行器の生成時に起動し、複数フレームで再利用し、実行器の破棄時に終了を待ちます。

対応するメンバ関数は void(float) noexcept です。ID に登録した型に一致する関数を渡してください。ForEach の呼び出し元は一つのスレッドに限定します。内部では添字範囲を分割し、GetBatch と開始・完了バリアを使います。型を接続するテンプレート部分は parallel_executor_impl.h、同期処理は parallel_executor.cpp に置いています。

キー入力、停止切替、フレーム待機、表示は benchmarks/interactive_test に置いています。main は InteractiveActions に生成・削除・更新・状態取得のコールバックを設定します。

1. 対話処理が主スレッドから生成・削除コールバックを呼ぶ。
2. 更新コールバック内の ForEach でコンポーネントを並列更新し、完了を待つ。
3. 対話処理が状態取得コールバックで値を受け取り、表示と次のフレームの待機を行う。
4. 対話処理が戻った後、main が実行器を終了させ、残る実体を破棄する。


取得済み span の使用中は、登録・追加・削除・Reserve・Clear・Pool 破棄を行わないでください。異なる実体を担当していても、Update が共有データや他の実体を書き換える場合は別途同期が必要です。

| キー | 操作 |
|---|---|
| E / B | Enemy / Bulletを1個生成 |
| 1 / 2 | Enemy / Bulletを64個生成 |
| D / F | 先頭のEnemy / Bulletを削除し、末尾のポインタを移す |
| C | 全実体を登録解除してdelete。Slabは再利用用に保持 |
| Space | 更新の一時停止・再開。停止中も追加・削除可能 |
| S | 状態表示 |
| Q / Esc | ワーカーを停止し、全実体を破棄して終了 |

コンソールでは Enter 不要、入力がない間も更新します。固定刻みは1/60秒で、実時間の60fpsは保証しません。入力リダイレクトでは検証用に1文字ずつ処理し、EOFで終了します。

~~~powershell
'eb12df s.cs12 q' | .\bin\x64\Release\slabPool.exe
~~~

## 時間比較

同じ new の実体を共有し、以下の三方式で同じ順序・同じ Update を実行します。

- 実体内の next を辿る双方向リンクリスト（previous は保持するが走査では使わない）。
- Slab に格納したポインタ列。
- std::vector<Item*> の順次走査（参考値）。

実体は64バイト。生成順と固定seedのランダム順、Slab容量4・8・64・256個（x64で32・64・512・2048バイト）を比較します。各条件の初期容量も同じ個数に揃えます。生成・登録・破棄、初期値復元、結果検証は計測外です。1周準備してから16周を測定し、方式の実行順を交代して中央値を出します。キャッシュを強制排出する条件ではなく、CPUのキャッシュミス率も測定しません。

初期容量を大きくしてSlab数を減らすこともできます。小さなSlabは、範囲取得や管理情報の負担が相対的に増えます。64バイト境界への整列後の短縮結果は docs/benchmark-pointer-aligned-quick.txt を参照してください。docs/benchmark-pointer-quick.txt は型付きビュー・整列導入前の履歴です。速くなる保証や、既存ゲーム全体の速度を示すものではありません。Slabが遅い条件やvectorの参考値も、そのまま表示します。

### 64バイトの単位と性能の見方

64ビットは8バイト、今回の追加容量は64バイトです。キャッシュラインが64バイトのCPUでは、64バイト境界から始まるポインタ8個が一つのラインに収まります。ただし、512バイトなどの大きなSlabも複数のラインに分かれて処理されます。ラインサイズにSlab容量を一致させることが最速という意味ではありません（[Intelのキャッシュラインの説明](https://www.intel.com/content/www/us/en/developer/articles/technical/loop-optimizations-where-blocks-are-required.html)）。

この実装ではSlabを取得するたびに索引を検索するため、小さいSlabは取得回数と管理領域が増えます。2026-10-13のRelease測定では、10万個・生成順・16周で、64バイトSlabが13.9087ms、512バイトSlabが3.2969msでした。同じ64バイト境界で容量だけを変えた結果です。初期容量は各Slab容量と同じで、大きな初期Slabと小さな追加Slabを混在させた測定ではありません。

既存のポインタ配列は拡張時に移動しませんが、新しいSlabの確保と、必要に応じた索引配列の再確保は発生します。上記の数値は走査時間です。追加・容量拡張の測定は次の節に分けています。ゲームのフレーム時間は未計測です。
## 追加・再確保の比較

benchmarks/growth_benchmark.cpp では、同じ生成済み実体のポインタを、空のコンテナへ1万個・10万個ずつ追加します。SLAB_FULL_BENCHMARK を有効にすると100万個も測定します。

- vector・reserveなし: 標準の容量拡張方針で push_back する。
- vector・全件reserveあり: 指定個数を reserve してから push_back する。
- Slab・初期0: Reserveを使わず、64・512・2048バイトずつ追加する。

各方式を一度慣らした後、実行順を交代して3回（拡張設定は5回）測定します。毎回新しい空のコンテナを作り、確保済みのコンテナを再利用しません。ヒープやCPUキャッシュを初期状態に戻す測定ではありません。

**全体測定**は、準備（vectorのreserveまたはSlabのID登録）、全ポインタの追加、準備＋追加の時間をそれぞれ記録します。追加ループの途中には時計の読み取りや拡張情報の記録を挟みません。各列は独立した中央値なので、準備と追加の表示値を足しても合計欄に一致しない場合があります。

**拡張診断**は別の走行で行い、容量が尽きた追加だけを時計で挟みます。初回確保を含む拡張回数、再配置回数、再配置対象ポインタ数、その追加にかかった合計時間・最大時間を記録します。確保、ポインタの再配置、旧領域の解放を含む一回の追加時間であり、コピー処理だけを分離した時間ではありません。再配置バイト数は旧要素数×sizeof(void*)の算出値で、実際のメモリ転送量ではありません。

Slabの登録済みポインタは再配置されませんが、Slab索引の再確保・コピーはAdd時間に含みます。索引のコピー個数は「登録ポインタの再配置数」には含めません。reserveありのvectorは追加中の拡張が0回なので、その拡張時間の表示は0です。これは通常追加や事前reserveの時間が0という意味ではありません。

時計の空測定は参考表示で、測定値から減算しません。分解能以下の短い区間は0と表示されるため、0は時計の読み取りが無コストという意味ではありません。最大値にはOSの割り込みやほかの処理の影響が含まれます。総時間と診断時間は別走行なので、互いに足し合わせて使いません。

実体の生成・終了時のコンテナ破棄・ポインタ列と実体の検証は計測外です。最新ログは [benchmark-pointer-growth-quick.txt](docs/benchmark-pointer-growth-quick.txt)、主要結果は [検証記録](docs/validation.md) にあります。
## 同時追加の使い方

AddConcurrent(id, pointer) は、IDごとの std::mutex を取得して通常の Add と同じ追加処理を行います。同じIDの登録数・Slab拡張・索引再確保・ポインタ書き込みを保護し、成功時はその追加の添字、失敗時は InvalidIndex を返します。実体は呼び出し前に構築してください。複数スレッド間の登録順は不定で、重複ポインタの検査は行いません。異なるIDは別のmutexを使用します。

1. 主スレッドで全IDの Register と必要な Reserve を済ませる。
2. 複数スレッドから AddConcurrent を呼ぶ。実体と所有権の書き込み先もスレッドごとに分ける。
3. 全スレッドの完了を待ってから、Count・At・GetBatch・GetStatistics・更新・削除・Clear・破棄を行う。

**同時実行できるのは AddConcurrent 同士です。** 通常の Add や他のAPIを同時に呼んではいけません。Add はロックのない経路として残しています。AddConcurrent はロックフリーではなく、同じIDへの追加そのものは直列になります。コンポーネント本体のデータ競合は利用側で防いでください。

main.cpp の ShowConcurrentUsage に、14ワーカーからEnemyをnewして同じIDへ112個追加し、完了後に型付き範囲でUpdateし、Clear後にdeleteする例があります。スレッド作成・範囲分割・完了待ちは benchmarks/parallel_tasks に分離しています。ForEachRange のコールバックは (workerIndex, first, last) を受け取り、[first, last) を担当します。戻るまでに全コールバックが完了し、以降はPoolを通常のAPIで扱えます。

## 複数スレッドでの追加比較

main の SLAB_RUN_PARALLEL_GROWTH_BENCHMARK で有効にします。1・10・14ワーカーそれぞれで、合計1万・10万件の追加を測定します。SLAB_FULL_BENCHMARK は100万件も追加します。スレッド数倍に件数を増やす測定ではありません。

| 条件 | コンテナ | 追加API | 比較する負担 |
|---|---|---|---|
| 独立・通常 | ワーカーごとに別 | Add / vector::push_back | ロックなしの追加 |
| 独立・同時追加 | ワーカーごとに別 | AddConcurrent / mutex付きpush_back | 競合のないロック取得 |
| 共有・同時追加 | 全ワーカーで一つ | AddConcurrent / mutex付きpush_back | 同じIDのロック競合 |

全条件でvectorのreserve有無と、初期0のSlab（64・512・2048バイト）を比較します。各方式を一度慣らし、方式順を交代して3回（拡張設定は5回）測定します。独立条件の容量は各ワーカーの担当件数に合わせ、後で一つのコンテナへ統合する処理は含めません。独立条件では再配置対象の配列自体も小さくなるので、共有条件との違いをロック待ちだけに帰属させることはできません。

ワーカーは起動完了を確認してから測定し、繰り返し使います。事前reserve・ID登録は主スレッドで行い、準備時間として別に記録します。追加時間は開始通知から全ワーカーの完了までの実時間で、バリア通知・待機・OSのスケジューリングを含みます。実体とコンテナの生成・破棄、結果検証は除外します。各列は独立した中央値です。

個々の追加の診断は別走行です。API呼び出しの前後だけで計り、AddConcurrent のロック待ち・追加・拡張・ロック解放を含めます。追加中にCountや統計を読み取ることや、計測用の別のmutexで直列化することはありません。共有条件の最大値を、再確保やポインタの移動だけにかかった時間とは解釈できません。拡張だけの診断と再配置対象数は、先述の単一スレッド測定で確認できます。

共有条件は登録順が不定なので、終了後に全ポインタの集合を重複数込みで照合します。独立条件は担当範囲と登録順も照合します。どちらも実体のアドレス・値は保持されます。CPU固定やキャッシュ排出は行わず、ヒープは走行間で再利用されます。OSの割り込みも最大時間に含まれます。ピーク時間の上限を保証する測定ではありません。

最新ログ: [benchmark-pointer-parallel-quick.txt](docs/benchmark-pointer-parallel-quick.txt)。実測の比較表と検証結果は [検証記録](docs/validation.md) を参照してください。
## ランダム削除・追加後の走査比較

main の SLAB_RUN_CHURN_BENCHMARK を有効にすると、benchmarks/churn_benchmark.cpp の比較を実行します。初期1万体・10万体で、次の周期を8回繰り返します。SLAB_FULL_BENCHMARK を有効にすると100万体も追加します。

1. 生存中の実体から10%を重複なしでランダムに選び、主スレッドで登録解除してdeleteする。
2. 削除した数の実体をnewし、コンテナの末尾へ追加する。
3. 1・10・14ワーカーで全生存実体を走査し、同じUpdateを1回ずつ実行する。
4. 全実体のID・添字・リンク・更新回数・計算結果、削除/登録漏れと重複を検証する。

通常Addでは追加を主スレッドで行い、AddConcurrentでは指定本数のワーカーから同じコンテナへ追加します。削除はどちらも主スレッドです。削除・追加・走査を同時に実行する測定ではなく、各段階の完了を待って次へ進みます。SlabPoolの同期契約は変更していません。

固定seedで削除対象のIDを先に作り、どの方式・スレッド数でも同じIDを削除します。実体は64バイトで、双方向リンクと添字のフィールドを全方式で持ちます。削除対象の実体ポインタは所有配列から取り出し、リンクリストでは直接リンクを外し、vector/Slabでは末尾交換後の実体の添字を修正します。vector::eraseによる後続全要素のシフトは行いません。

比較対象は、双方向リンクリスト、初期reserveなし/最大件数reserveありのvector、64・512・2048バイトSlabです。リンクリストとvectorの同時追加にも1回ごとのmutexを使用します。リンクリストの並列走査は、各担当範囲の先頭を事前にたどる必要があり、この分割準備時間も走査時間へ含めて表示します。

削除時間には登録解除・添字修正・deleteを、追加時間にはnew・構築・末尾登録を含めます。走査時間には範囲分割・ワーカーへの通知・完了待ち・全件Updateを含めます。初期の構築とreserve、乱数生成、スレッド作成、各周期の検証、終了時のコンテナ/実体破棄は除外します。変更前の初期走査も参考表示しますが、周期合計には含めません。

各方式を1走行慣らし、方式順を交代して3走行（拡張設定は5走行）測ります。表示する各時間は「8周期の平均を走行ごとに求めた値の中央値」です。各列は独立した中央値なので、表示された内訳の和が合計欄に一致しない場合があります。周期合計は3つの計測区間の和で、実際のゲームフレーム全体の時間ではありません。周期最大値も記録します。

同数補充では確保済み容量を使い回すため、初期reserveなしのvectorも周期中は通常再確保しません。main の SLAB_CHURN_GROWING を有効にすると、初期個数の10%削除・20%追加へ切り替え、8周期後に1.8倍まで増やせます。reserveありのvectorは最大件数を初期に予約します。どちらの条件も乱数選択はその時点の全生存実体が対象です。

各方式は新しい実体をnewし直すため、同じIDでも物理アドレスの一致は保証しません。リンクリストは残存順を保ち、vector/Slabは末尾交換し、同時追加の順序は不定です。各周期後の全件検証でもキャッシュが温まる条件であり、キャッシュミス率自体の計測ではありません。削除前後の走査時間の差を、そのままキャッシュだけの効果とは解釈できません。

測定ログ: [benchmark-pointer-churn-quick.txt](docs/benchmark-pointer-churn-quick.txt)。比較表は [検証記録](docs/validation.md) にあります。
## ビルド・実行

slabPool.slnx を Visual Studio 2026 で開いて Ctrl+F5。「自動テスト → 一括登録・並列更新の使用例 → 一括確保・コピー・並列走査の比較」の順に実行して終了します。C++20 / MSVC v145 / Windows SDK が必要です。

main.cpp の define で切り替えます。実行時引数は不要です。

| define | 既定 | 動作 |
|---|---|---|
| SLAB_RUN_TESTS | 有効 | 自動テスト |
| SLAB_RUN_BULK_BENCHMARK | 有効 | 一括確保・ポインタコピー・並列走査の比較 |
| SLAB_TESTS_ONLY | 無効 | 自動テストだけを実行 |
| SLAB_RUN_BENCHMARKS | 無効 | 1万個・10万個、各3回の走査比較 |
| SLAB_RUN_GROWTH_BENCHMARK | 無効 | reserve有無の追加・容量拡張比較 |
| SLAB_RUN_PARALLEL_GROWTH_BENCHMARK | 無効 | 1・10・14ワーカーで通常追加・同時追加を比較 |
| SLAB_RUN_CHURN_BENCHMARK | 無効 | ランダム削除・new/末尾追加・並列走査を比較 |
| SLAB_CHURN_GROWING | 無効 | 変更比較を10%削除・20%追加の増加条件にする |
| SLAB_RUN_INTERACTIVE_TEST | 無効 | キー入力と並列Update |
| SLAB_FULL_BENCHMARK | 無効 | 100万個を追加、各5回に変更 |

~~~powershell
.\scripts\build.ps1 -Configuration Both
.\scripts\build.ps1 -Configuration Release -Run
.\scripts\build.ps1 -Configuration Debug -Sanitize -TestsOnly -Run
~~~

Releaseの結果を性能判断に使用してください。検証記録は docs/validation.md にあります。benchmark-numeric-quick.txt とそれ以前のログは、実体をSlab内に構築していた旧実装の履歴です。現在の比較結果とは異なります。
