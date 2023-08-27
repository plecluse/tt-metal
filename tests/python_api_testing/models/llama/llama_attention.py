import sys
import math
import torch
from torch import nn
import tt_lib
from typing import Optional, Tuple
from models.utility_functions import torch2tt_tensor, tt2torch_tensor, pad_by_zero
from models.helper_funcs import Linear as TTLinear


def shape_tt(states, batch_size, seq_len, n_heads, head_dim):
    tt_out = tt_lib.tensor.reshape(states, batch_size, seq_len, n_heads, head_dim)
    tt_out = tt_lib.tensor.transpose_hc(tt_out)

    return tt_out


def shape_pt(tensor: torch.Tensor, seq_len: int, bsz: int):
    num_heads = 32
    head_dim = 512
    return tensor.view(bsz, seq_len, num_heads, head_dim).transpose(1, 2).contiguous()


def test_lamma_shape(device):
    batch_size = 1
    n_heads = 32
    seq_len = 128
    head_dim = 512

    torch.manual_seed(0)
    test_input = (torch.rand(1, 32, 128, 512) * 2) - 1
    test_input = test_input.to(torch.float16)

    pt_out = shape_pt(test_input, seq_len, batch_size)

    test = torch2tt_tensor(test_input, device)
    tt_out = shape_tt(test, batch_size, seq_len, n_heads, head_dim)
    tt_out = tt2torch_tensor(tt_out)

    if np.allclose(pt_out.detach().numpy(), tt_out, 1e-4, 0.17):
        logger.info("llama_shape test Passed!")
    else:
        logger.warning("llama_shape test Failed!")


class LlamaRotaryEmbedding(torch.nn.Module):
    def __init__(self, dim, max_position_embeddings=2048, base=10000, device=None):
        super().__init__()
        inv_freq = 1.0 / (base ** (torch.arange(0, dim, 2).float().to(device) / dim))
        self.register_buffer("inv_freq", inv_freq)

        # Build here to make `torch.jit.trace` work.
        self.max_seq_len_cached = max_position_embeddings
        t = torch.arange(
            self.max_seq_len_cached,
            device=self.inv_freq.device,
            dtype=self.inv_freq.dtype,
        )
        freqs = torch.einsum("i,j->ij", t, self.inv_freq)
        # Different from paper, but it uses a different permutation in order to obtain the same calculation
        emb = torch.cat((freqs, freqs), dim=-1)
        self.register_buffer(
            "cos_cached", emb.cos()[None, None, :, :], persistent=False
        )
        self.register_buffer(
            "sin_cached", emb.sin()[None, None, :, :], persistent=False
        )

    def forward(self, x, seq_len=None):
        # x: [bs, num_attention_heads, seq_len, head_size]
        # This `if` block is unlikely to be run after we build sin/cos in `__init__`. Keep the logic here just in case.
        if seq_len > self.max_seq_len_cached:
            self.max_seq_len_cached = seq_len
            t = torch.arange(
                self.max_seq_len_cached, device=x.device, dtype=self.inv_freq.dtype
            )
            freqs = torch.einsum("i,j->ij", t, self.inv_freq)
            # Different from paper, but it uses a different permutation in order to obtain the same calculation
            emb = torch.cat((freqs, freqs), dim=-1).to(x.device)
            self.register_buffer(
                "cos_cached", emb.cos()[None, None, :, :], persistent=False
            )
            self.register_buffer(
                "sin_cached", emb.sin()[None, None, :, :], persistent=False
            )
        return (
            self.cos_cached[:, :, :seq_len, ...].to(dtype=x.dtype),
            self.sin_cached[:, :, :seq_len, ...].to(dtype=x.dtype),
        )


def rotate_half(x):
    """Rotates half the hidden dims of the input."""
    x1 = x[..., : x.shape[-1] // 2]
    x2 = x[..., x.shape[-1] // 2 :]
    return torch.cat((-x2, x1), dim=-1)


def apply_rotary_pos_emb(q, k, cos, sin, position_ids):
    # The first two dimensions of cos and sin are always 1, so we can `squeeze` them.
    cos = cos.squeeze(1).squeeze(0)  # [seq_len, dim]
    sin = sin.squeeze(1).squeeze(0)  # [seq_len, dim]
    cos = cos[position_ids].unsqueeze(1)  # [bs, 1, seq_len, dim]
    sin = sin[position_ids].unsqueeze(1)  # [bs, 1, seq_len, dim]
    q_embed = (q * cos) + (rotate_half(q) * sin)
    k_embed = (k * cos) + (rotate_half(k) * sin)
    return q_embed, k_embed


class TtLlamaAttention(nn.Module):
    """Multi-headed attention from 'Attention Is All You Need' paper"""

    def __init__(
        self,
        device,
        base_url,
        state_dict,
        layer_num,
        hidden_size: int,
        num_heads: int,
        max_position_embeddings: int = 2048,
    ):
        super().__init__()
        self.hidden_size = hidden_size
        self.num_heads = num_heads
        self.head_dim = hidden_size // num_heads
        self.max_position_embeddings = max_position_embeddings
        self.device = device
        self.state_dict = state_dict

        if (self.head_dim * num_heads) != self.hidden_size:
            raise ValueError(
                f"hidden_size must be divisible by num_heads (got `hidden_size`: {self.hidden_size}"
                f" and `num_heads`: {num_heads})."
            )

        self.q_weights = torch2tt_tensor(
            self.state_dict[f"{base_url}.{layer_num}.self_attn.q_proj.weight"],
            self.device,
        )
        self.k_weights = torch2tt_tensor(
            self.state_dict[f"{base_url}.{layer_num}.self_attn.k_proj.weight"],
            self.device,
        )
        self.v_weights = torch2tt_tensor(
            self.state_dict[f"{base_url}.{layer_num}.self_attn.v_proj.weight"],
            self.device,
        )
        self.o_weights = torch2tt_tensor(
            self.state_dict[f"{base_url}.{layer_num}.self_attn.o_proj.weight"],
            self.device,
        )

        self.rotary_emb = LlamaRotaryEmbedding(
            self.head_dim, max_position_embeddings=self.max_position_embeddings
        )

        self.query_linear = TTLinear(
            self.q_weights.shape()[-1], self.q_weights.shape()[-2], self.q_weights
        )
        self.key_linear = TTLinear(
            self.k_weights.shape()[-1], self.k_weights.shape()[-2], self.k_weights
        )
        self.value_linear = TTLinear(
            self.v_weights.shape()[-1], self.v_weights.shape()[-2], self.v_weights
        )
        self.attn_linear = TTLinear(
            self.o_weights.shape()[-1], self.o_weights.shape()[-2], self.o_weights
        )

        self.scalar = pad_by_zero(torch.Tensor([1/math.sqrt(self.head_dim)]), self.device)[0]

    def _shape(self, tensor: torch.Tensor, seq_len: int, bsz: int):
        return (
            tensor.view(bsz, seq_len, self.num_heads, self.head_dim)
            .transpose(1, 2)
            .contiguous()
        )
        # return shape_tt(tensor, bsz, seq_len, self.num_heads, self.head_dim)

    def forward(
        self,
        hidden_states: tt_lib.tensor.Tensor,
        attention_mask: Optional[torch.Tensor] = None,
        position_ids: Optional[torch.LongTensor] = None,
        past_key_value: Optional[Tuple[torch.Tensor]] = None,
        output_attentions: bool = False,
        use_cache: bool = False,
    ) -> Tuple[torch.Tensor, Optional[torch.Tensor], Optional[Tuple[torch.Tensor]]]:
        """Input shape: Batch x Time x Channel"""

        bsz = hidden_states.shape()[0]
        q_len = hidden_states.shape()[2]
        query = self.query_linear(hidden_states)
        query_states = shape_tt(query, bsz, q_len, self.num_heads, self.head_dim)

        key = self.key_linear(hidden_states)
        key_states = shape_tt(key, bsz, q_len, self.num_heads, self.head_dim)

        value = self.value_linear(hidden_states)
        value_states = shape_tt(value, bsz, q_len, self.num_heads, self.head_dim)

        # return all states to pytorch =============================
        query_states = tt2torch_tensor(query_states)
        key_states = tt2torch_tensor(key_states)
        value_states = tt2torch_tensor(value_states)
        # return all states to pytorch =============================

        # get positions_ids values if it is None
        seq_length = q_len
        past_key_values_length = 0
        if position_ids is None:
            position_ids = torch.arange(
                past_key_values_length,
                seq_length + past_key_values_length,
                dtype=torch.long,
                device=None,
            )
            position_ids = position_ids.unsqueeze(0).view(-1, seq_length)
        else:
            position_ids = position_ids.view(-1, seq_length).long()

        kv_seq_len = key_states.shape[-2]
        if past_key_value is not None:
            kv_seq_len += past_key_value[0].shape[-2]
        cos, sin = self.rotary_emb(value_states, seq_len=kv_seq_len)
        query_states, key_states = apply_rotary_pos_emb(
            query_states, key_states, cos, sin, position_ids
        )

        if past_key_value is not None:
            # reuse k, v, self_attention
            key_states = tt_lib.fallback_ops.concat(
                [past_key_value[0], key_states], dim=2
            )
            value_states = tt_lib.fallback_ops.concat(
                [past_key_value[1], value_states], dim=2
            )

        past_key_value = (key_states, value_states) if use_cache else None

        # TT implementation for:
        # attn_weights = torch.matmul(query_states, key_states.transpose(2, 3)) / math.sqrt(self.head_dim)
        key_states_tt = torch2tt_tensor(key_states, self.device)
        query_states_tt = torch2tt_tensor(query_states, self.device)

        key_states_tt_transposed = tt_lib.tensor.transpose(key_states_tt)
        mul = tt_lib.tensor.bmm(query_states_tt, key_states_tt_transposed)

        # TODO: Fuse into softmax
        attn_weights = tt_lib.tensor.bcast(mul, self.scalar, tt_lib.tensor.BcastOpMath.MUL, tt_lib.tensor.BcastOpDim.HW)

        if attn_weights.shape() != [bsz, self.num_heads, q_len, kv_seq_len]:
            raise ValueError(
                f"Attention weights should be of size {(bsz * self.num_heads, q_len, kv_seq_len)}, but is"
                f" {attn_weights.shape()}"
            )

        # change attention_mask to TT tensor
        if attention_mask is not None:
            if attention_mask.size() != (bsz, 1, q_len, kv_seq_len):
                raise ValueError(
                    f"Attention mask should be of size {(bsz, 1, q_len, kv_seq_len)}, but is {attention_mask.shape()}"
                )
            # TT eltwise add operation, expand attention_mask shape
            attention_mask = attention_mask.repeat(1, self.num_heads, 1, 1)
            attention_mask = torch2tt_tensor(attention_mask, self.device)
            attn_weights = tt_lib.tensor.add(attn_weights, attention_mask)
            # convert to PyTorch tensor
            attn_weights = tt2torch_tensor(attn_weights)
            attn_weights = torch.max(
                attn_weights, torch.tensor(torch.finfo(attn_weights.dtype).min)
            )

        # TT implementation for:
        # PyTorch: upcast attention to fp32
        # attn_weights = nn.functional.softmax(attn_weights, dim=-1, dtype=torch.float32).to(query_states.dtype)
        # attn_output = torch.matmul(attn_weights, value_states)

        if not isinstance(attn_weights, tt_lib.tensor.Tensor):
            attn_weights = torch2tt_tensor(attn_weights, self.device)
        value_states = torch2tt_tensor(value_states, self.device)

        # torch softmax
        attn_weights = tt_lib.operations.primary.softmax_in_place(attn_weights)
        attn_output = tt_lib.tensor.bmm(attn_weights, value_states)

        if attn_output.shape() != [bsz, self.num_heads, q_len, self.head_dim]:
            raise ValueError(
                f"`attn_output` should be of size {(bsz, self.num_heads, q_len, self.head_dim)}, but is"
                f" {attn_output.shape()}"
            )

        attn_output = tt_lib.tensor.transpose_hc(attn_output)
        attn_output = tt_lib.tensor.reshape(
            attn_output, bsz, 1, q_len, self.hidden_size
        )
        attn_output = self.attn_linear(attn_output)

        if not output_attentions:
            attn_weights = None

        return attn_output, attn_weights, past_key_value
