# C++20 数値ID別Slab Pool

外部で生成したコンポーネントのポインタを、数値IDごとに保持する非所有プールです。実体の生成・解放は利用側が担当します。

ID管理は`Registry`内の`std::unordered_map<PoolId, Group>`が担当し、Groupをmapの要素として直接保持します。Registry自体はSlabPoolの直接メンバーとして最初から保持し、make_uniqueによる個別生成は行いません。Registry・Groupの定義はヘッダー下部のprivate部分、関数の実装はcppにまとめています。

初回登録時とClear後の再登録時に`InitialGroupCapacity`個のIDを想定してバケットをreserveします。既定値は64で、必要ならslab_pool.cpp冒頭の数値を変更します。GroupやSlab領域まで事前生成する設定ではありません。map・listの内部領域は通常どおり動的に確保します。

IDが想定数を超えると登録時に再ハッシュすることがあります。Groupへのポインタは再ハッシュでも有効ですが、登録・Clearは全利用者の停止中に行います。全IDの処理順は登録順にはなりません。

予約領域の管理情報は`list<Region>`、コミット区間の管理情報は`list<Chunk>`で保持します。前後のリンクや管理ノードの解放はlistに任せ、各Regionのデストラクタが仮想領域を解放します。コンポーネントのポインタ配列は仮想領域内で連続配置し、Rangeはlistのイテレータで区間を移動します。

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

1. `slab::Component`を継承したクラスを`Register<T>(id)`で登録する。
2. 外側で実体を生成し、`Add(id, Component*)`でポインタを1件ずつ渡す。Addはテンプレートではなく、Enemy*なども基底ポインタへ暗黙変換して渡せる。
3. `GetRange<T>(id)`の取得成功をboolで確認し、範囲forで処理する。
4. 除去する実体の`state`を0にして、`NotifyStateChanged(id)`を呼ぶ。
5. 全IDの処理とRangeの利用終了後、`RemoveInactive()`を呼ぶ。
6. 除去された実体を外側で解放する。終了時は`Clear()`で全IDの登録と格納メモリを解放してから、残る実体も外側で解放する。

追加時には実体の派生型を照合しません。`Register<Enemy>(enemyId)`で登録したIDにはEnemyの実体を渡してください。`GetRange<T>(id)`が照合するのはTとIDの登録型であり、追加された実体の型ではありません。

`RemoveInactive()`は変更フラグのあるIDだけを走査し、状態0のポインタを末尾と入れ替えて除去します。順序・添字は変わりますが、実体は移動・解放しません。

`Clear()`は全IDのポインタ・登録情報・管理情報・予約とコミット済みの領域を解放します。コンポーネント実体は解放しません。Rangeが残っていれば何も変更せずfalseを返します。空Poolにも繰り返し呼べて、再利用時はRegisterからやり直します。

Clearでは内部のmapを空のmapと交換し、登録に伴って確保したノードとバケット領域を解放します。Group内のlistが管理情報を破棄し、Regionのデストラクタが各Slabの予約・コミット済み領域を解放します。Registry自体はメンバーとして残り、mapは空の初期状態へ戻ります。

要素の取得にはRangeの範囲forを使います。`At()`による添字アクセスはありません。Rangeの破棄・Resetで走査権を解放し、その後は取得した参照やイテレータを利用しません。

## 並列利用

同じIDは同時に一つのRangeだけが利用できます。使用中の取得は待たずに失敗し、Rangeの破棄・`Reset()`後は別スレッドが取得できます。空IDでも取得成功ならboolはtrueです。

別IDは並列利用できます。`state`は普通のintなので、並列処理中はそのIDのRangeを持つ担当だけが実体を読み書きします。同じ実体を複数IDへ登録しないでください。参照・ポインタ・イテレータはRangeの解放前に使い終えます。

Poolはスレッドを起動しません。登録・Clear・Poolの破棄は全利用者の停止中に行い、一括除去の前には外側で全ジョブの完了を待ちます。ClearはIDの管理情報も破棄するため、同時に新しいGetRange・Add・通知などを開始しないでください。

## 容量設定

ページサイズとポインタサイズは、slab_pool.cpp冒頭の数値で指定します。SYSTEM_INFOによる実行時の取得は行いません。

```cpp
constexpr std::size_t PageBytes{4096};
constexpr std::size_t PointerBytes{8};
```

PageBytesは対象OSの実際の通常ページサイズに合わせます。OS自体のページサイズを変更する設定ではありません。PointerBytesは実際のComponent*のサイズと一致することをstatic_assertで確認します。

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
