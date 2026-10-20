# C++20 数値ID別Slab Pool

外部で生成したコンポーネントのポインタを、数値IDごとに保持する非所有プールです。実体の生成・解放は利用側が担当します。

ID管理は`Registry`内の`std::unordered_map<PoolId, Group>`が担当し、Groupをmapの要素として直接保持します。Registry自体はSlabPoolの直接メンバーとして最初から保持し、make_uniqueによる個別生成は行いません。Registry・Groupの定義はヘッダー下部のprivate部分、関数の実装はcppにまとめています。

初回登録時とClear後の再登録時に`InitialGroupCapacity`個のIDを想定してバケットをreserveします。既定値は64で、必要ならslab_pool.cpp冒頭の数値を変更します。GroupやSlab領域まで事前生成する設定ではありません。map・listの内部領域は通常どおり動的に確保します。

登録済みIDの上書きを防いだうえで、`groups[id]`で空のGroupを作り、容量設定を代入します。型タグは保持せず、同じIDへの再登録はfalseを返します。

IDが想定数を超えると登録時に再ハッシュすることがあります。Groupへのポインタは再ハッシュでも有効ですが、登録・Clearは全利用者の停止中に行います。全IDの処理順は登録順にはなりません。

予約領域の管理情報は`list<Region>`、コミット区間の管理情報は`list<Chunk>`で保持します。前後のリンクや管理ノードの解放はlistに任せ、各Regionのデストラクタが仮想領域を解放します。コンポーネントのポインタ配列は仮想領域内で連続配置し、Rangeはlistのイテレータで区間を移動します。

走査用の`Slice`は参照開始位置・区間内の枠数・次のChunkだけを保持します。イテレータは状態0も含めた残り格納数で終了を判断し、0件のIDでは最初のChunkも参照しません。内部の`ReadChunk`は有効なChunkだけを受け取り、終端の検査を行いません。この前提は、走査中の追加・除去を排他し、格納数を維持することで保証します。

## 構成

| ファイル | 内容 |
|---|---|
| [main.cpp](main.cpp) | 登録・生成・追加・走査・除去・解放の最小例 |
| [slab_pool.h](include/slab/slab_pool.h) | 共通基底と公開インターフェース。末尾にテンプレート実装 |
| [slab_pool.cpp](include/slab/slab_pool.cpp) | 仮想メモリ管理・ID単位の排他・末尾入れ替え除去 |

## 使い方

`SlabPool::GetInstance()`で共有インスタンスを取得できます。利用側でPoolを生成する必要はありません。

```cpp
auto& pool{slab::SlabPool::GetInstance()};
```

共有インスタンスは初回呼び出し時に一度だけ生成され、プログラム終了時に破棄されます。初期化はスレッドセーフですが、登録や走査などの利用条件は下記のとおりです。コンストラクタは公開しているので、必要なら`slab::SlabPool localPool{};`で独立したPoolも作れます。

1. `slab::Component`を継承したクラスごとにIDを決め、`Register(id, options)`で格納領域を登録する。Registerはテンプレートではない。
2. 外側で実体を生成し、`Add(id, Component*)`でポインタを1件ずつ渡す。Addはテンプレートではなく、Enemy*なども基底ポインタへ暗黙変換して渡せる。
3. `GetRange<T>(id)`の取得成功をboolで確認し、範囲forで処理する。
4. 除去する実体の`state`を0にして、`NotifyStateChanged(id)`を呼ぶ。
5. 全IDの処理とRangeの利用終了後、`RemoveInactive()`を呼ぶ。
6. 除去された実体を外側で解放する。終了時は`Clear()`で全IDの登録と格納メモリを解放してから、残る実体も外側で解放する。

IDとクラスは利用側で一対一に対応させます。Enemy用のIDにはEnemyの実体を追加し、`GetRange<Enemy>(enemyId)`で走査してください。Poolは型情報を保持せず、Add・GetRangeとも実行時の型照合を行いません。異なる型を指定して走査することは未定義動作になります。Componentの継承条件とvolatileの禁止はコンパイル時に確認するため、実行時のコストはありません。

未登録ID・nullptr・容量不足や確保失敗は、従来どおりfalse・無効なRange・InvalidIndexで通知します。GetRange・Add・CommitAtFrameEndは同じIDが使用中なら待機し、取得できてから処理します。

`RemoveInactive()`は変更フラグのあるIDだけを走査し、状態0のポインタを末尾と入れ替えて除去します。順序・添字は変わりますが、実体は移動・解放しません。

`Clear()`は全IDのポインタ・登録情報・管理情報・予約とコミット済みの領域を解放します。コンポーネント実体は解放しません。Rangeが残っていれば何も変更せずfalseを返します。空Poolにも繰り返し呼べて、再利用時はRegisterからやり直します。

Clearでは内部のmapを空のmapと交換し、登録に伴って確保したノードとバケット領域を解放します。Group内のlistが管理情報を破棄し、Regionのデストラクタが各Slabの予約・コミット済み領域を解放します。Registry自体はメンバーとして残り、mapは空の初期状態へ戻ります。

要素の取得にはRangeの範囲forを使います。`At()`による添字アクセスはありません。Rangeの破棄・Resetで走査権を解放し、その後は取得した参照やイテレータを利用しません。

## 並列利用

同じIDは同時に一つのRangeだけが利用できます。GetRange・Add・CommitAtFrameEndは、同じIDが使用中なら`atomic::wait`で待ち、解放後に取得を再試行します。Rangeの破棄・`Reset()`や追加処理の終了時に、`notify_one()`で待機者へ通知します。空IDでも取得成功ならboolはtrueです。未登録IDへの操作は待機せず失敗します。

内部の待機式`Acquire()`は`compare_exchange_weak`を繰り返します。失敗時に書き換わるexpectedは試行ごとにfalseへ戻し、CASの成功時に前の所有者の更新を受け取ります。先着順や待ち時間の上限は保証しません。所有者の解放を待つ排他処理なので、ロックフリーではありません。

Rangeを保持したまま同じIDのGetRange・Add・CommitAtFrameEndを呼ぶと、自分の解放を待ち続けます。先にRangeを破棄・Resetしてから呼んでください。待機中のスレッドをjoinする場合も、先に自分のRangeを解放します。複数IDのRangeを同時に保持する場合は、全スレッドで取得順を統一します。

待機後のGetRangeもID全体を返すため、各呼び出しはそれぞれ全件を走査します。同じIDの処理は直列となり、別IDは並列に処理できます。

別IDは並列利用できます。`state`は普通のintなので、並列処理中はそのIDのRangeを持つ担当だけが実体を読み書きします。同じ実体を複数IDへ登録しないでください。参照・ポインタ・イテレータはRangeの解放前に使い終えます。

Poolはスレッドを起動しません。登録・Clear・Poolの破棄は待機者を含む全利用者の停止中に行い、一括除去の前には外側で全ジョブの完了を待ちます。ClearはIDの管理情報も破棄するため、同時に新しいGetRange・Add・通知などを開始しないでください。

Clear・RemoveInactiveは管理処理用の`TryAcquire()`を使い、使用中のIDがあれば待機せず失敗します。取得済みの権利を戻し、格納内容と変更フラグは維持します。これらの呼び出しで待機中のジョブの完了を代用することはできません。

## 容量設定

ページサイズは`GetSystemInfo`の`SYSTEM_INFO::dwPageSize`から初回だけ取得し、全Poolで共有します。予約・コミット量の切り上げとpreTouchの間隔に使い、範囲forの走査時は取得しません。ポインタサイズは`sizeof(Component*)`でコンパイル時に決めます。

予約時は`VirtualAlloc(nullptr, ...)`に先頭アドレスの選択を任せるため、予約アドレスの割り当て粒度への整列はOSが行います。ページサイズと割り当て粒度は別の値です。

`initialCapacity`と`reservedCapacity`はポインタの個数、`commitBytes`は追加コミット量です。仮想メモリを予約し、必要部分をOSページ単位でコミットします。予約を使い切ると別領域を追加し、既存のポインタ配列全体は移動しません。

実際の予約・コミットは非公開の`ReserveGroup`・`CommitGroup`が担当します。外側は登録時のOptionsで容量を指定します。

既定の`freeThreshold = 0`では追加時に不足分を自動コミットします。1以上なら準備済み容量内だけで追加し、フレーム末尾に`CommitAtFrameEnd(id)`を呼ぶと、空きが閾値以下の場合に`commitBytes`の1単位を追加します。

try/catchは使っていません。登録設定の不正やSlab用の仮想メモリの確保失敗はfalseを返します。map・listなど標準ライブラリによる管理情報の確保失敗は、noexcept境界で終了扱いになります。予約・コミットの失敗時は、直前に作ったlistの管理ノードを取り除きます。

## ビルド・実行

Visual Studio 2026・v145・C++20・x64で`slabPool.slnx`を開いて実行できます。実行時引数は不要です。

```powershell
.\scripts\build.ps1 -Configuration Both -Run
```

過去のベンチマーク・テスト・測定記録は`archive/id_range_validation`に退避しています。当時のAPI向けの記録で、ソリューションには含めていません。
